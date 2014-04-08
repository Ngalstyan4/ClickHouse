#include <DB/Storages/StorageReplicatedMergeTree.h>
#include <DB/Storages/MergeTree/ReplicatedMergeTreeBlockOutputStream.h>
#include <DB/Storages/MergeTree/ReplicatedMergeTreePartsExchange.h>
#include <DB/Parsers/formatAST.h>
#include <DB/IO/WriteBufferFromOStream.h>
#include <DB/IO/ReadBufferFromString.h>

namespace DB
{


const auto QUEUE_UPDATE_SLEEP = std::chrono::seconds(5);
const auto QUEUE_NO_WORK_SLEEP = std::chrono::seconds(5);
const auto QUEUE_ERROR_SLEEP = std::chrono::seconds(1);
const auto QUEUE_AFTER_WORK_SLEEP = std::chrono::seconds(0);
const auto MERGE_SELECTING_SLEEP = std::chrono::seconds(5);


StorageReplicatedMergeTree::StorageReplicatedMergeTree(
	const String & zookeeper_path_,
	const String & replica_name_,
	bool attach,
	const String & path_, const String & name_, NamesAndTypesListPtr columns_,
	Context & context_,
	ASTPtr & primary_expr_ast_,
	const String & date_column_name_,
	const ASTPtr & sampling_expression_,
	size_t index_granularity_,
	MergeTreeData::Mode mode_,
	const String & sign_column_,
	const MergeTreeSettings & settings_)
	:
	context(context_), zookeeper(context.getZooKeeper()),
	table_name(name_), full_path(path_ + escapeForFileName(table_name) + '/'), zookeeper_path(zookeeper_path_),
	replica_name(replica_name_), is_leader_node(false),
	data(	full_path, columns_, context_, primary_expr_ast_, date_column_name_, sampling_expression_,
			index_granularity_,mode_, sign_column_, settings_),
	reader(data), writer(data), merger(data), fetcher(data),
	log(&Logger::get("StorageReplicatedMergeTree: " + table_name)),
	shutdown_called(false)
{
	if (!zookeeper_path.empty() && *zookeeper_path.rbegin() == '/')
		zookeeper_path.erase(zookeeper_path.end() - 1);
	replica_path = zookeeper_path + "/replicas/" + replica_name;

	if (!attach)
	{
		if (!zookeeper.exists(zookeeper_path))
			createTable();

		if (!isTableEmpty())
			throw Exception("Can't add new replica to non-empty table", ErrorCodes::ADDING_REPLICA_TO_NON_EMPTY_TABLE);

		checkTableStructure();
		createReplica();
	}
	else
	{
		checkTableStructure();
		checkParts();
	}

	loadQueue();
	activateReplica();

	leader_election = new zkutil::LeaderElection(zookeeper_path + "/leader_election", zookeeper,
		std::bind(&StorageReplicatedMergeTree::becomeLeader, this), replica_name);

	queue_updating_thread = std::thread(&StorageReplicatedMergeTree::queueUpdatingThread, this);
	for (size_t i = 0; i < settings_.replication_threads; ++i)
		queue_threads.push_back(std::thread(&StorageReplicatedMergeTree::queueThread, this));
}

StoragePtr StorageReplicatedMergeTree::create(
	const String & zookeeper_path_,
	const String & replica_name_,
	bool attach,
	const String & path_, const String & name_, NamesAndTypesListPtr columns_,
	Context & context_,
	ASTPtr & primary_expr_ast_,
	const String & date_column_name_,
	const ASTPtr & sampling_expression_,
	size_t index_granularity_,
	MergeTreeData::Mode mode_,
	const String & sign_column_,
	const MergeTreeSettings & settings_)
{
	StorageReplicatedMergeTree * res = new StorageReplicatedMergeTree(zookeeper_path_, replica_name_, attach,
		path_, name_, columns_, context_, primary_expr_ast_, date_column_name_, sampling_expression_,
		index_granularity_, mode_, sign_column_, settings_);
	StoragePtr res_ptr = res->thisPtr();
	String endpoint_name = "ReplicatedMergeTree:" + res->replica_path;
	InterserverIOEndpointPtr endpoint = new ReplicatedMergeTreePartsServer(res->data, res_ptr);
	res->endpoint_holder = new InterserverIOEndpointHolder(endpoint_name, endpoint, res->context.getInterserverIOHandler());
	return res_ptr;
}

static String formattedAST(const ASTPtr & ast)
{
	if (!ast)
		return "";
	std::stringstream ss;
	formatAST(*ast, ss, 0, false, true);
	return ss.str();
}

void StorageReplicatedMergeTree::createTable()
{
	zookeeper.create(zookeeper_path, "", zkutil::CreateMode::Persistent);

	/// Запишем метаданные таблицы, чтобы реплики могли сверять с ними свою локальную структуру таблицы.
	std::stringstream metadata;
	metadata << "metadata format version: 1" << std::endl;
	metadata << "date column: " << data.date_column_name << std::endl;
	metadata << "sampling expression: " << formattedAST(data.sampling_expression) << std::endl;
	metadata << "index granularity: " << data.index_granularity << std::endl;
	metadata << "mode: " << static_cast<int>(data.mode) << std::endl;
	metadata << "sign column: " << data.sign_column << std::endl;
	metadata << "primary key: " << formattedAST(data.primary_expr_ast) << std::endl;
	metadata << "columns:" << std::endl;
	WriteBufferFromOStream buf(metadata);
	for (auto & it : data.getColumnsList())
	{
		writeBackQuotedString(it.first, buf);
		writeChar(' ', buf);
		writeString(it.second->getName(), buf);
		writeChar('\n', buf);
	}
	buf.next();

	zookeeper.create(zookeeper_path + "/metadata", metadata.str(), zkutil::CreateMode::Persistent);

	zookeeper.create(zookeeper_path + "/replicas", "", zkutil::CreateMode::Persistent);
	zookeeper.create(zookeeper_path + "/blocks", "", zkutil::CreateMode::Persistent);
	zookeeper.create(zookeeper_path + "/block_numbers", "", zkutil::CreateMode::Persistent);
	zookeeper.create(zookeeper_path + "/leader_election", "", zkutil::CreateMode::Persistent);
	zookeeper.create(zookeeper_path + "/temp", "", zkutil::CreateMode::Persistent);
}

/** Проверить, что список столбцов и настройки таблицы совпадают с указанными в ZK (/metadata).
	* Если нет - бросить исключение.
	*/
void StorageReplicatedMergeTree::checkTableStructure()
{
	String metadata_str = zookeeper.get(zookeeper_path + "/metadata");
	ReadBufferFromString buf(metadata_str);
	assertString("metadata format version: 1", buf);
	assertString("\ndate column: ", buf);
	assertString(data.date_column_name, buf);
	assertString("\nsampling expression: ", buf);
	assertString(formattedAST(data.sampling_expression), buf);
	assertString("\nindex granularity: ", buf);
	assertString(toString(data.index_granularity), buf);
	assertString("\nmode: ", buf);
	assertString(toString(static_cast<int>(data.mode)), buf);
	assertString("\nsign column: ", buf);
	assertString(data.sign_column, buf);
	assertString("\nprimary key: ", buf);
	assertString(formattedAST(data.primary_expr_ast), buf);
	assertString("\ncolumns:\n", buf);
	for (auto & it : data.getColumnsList())
	{
		String name;
		readBackQuotedString(name, buf);
		if (name != it.first)
			throw Exception("Unexpected column name in ZooKeeper: expected " + it.first + ", found " + name,
				ErrorCodes::UNKNOWN_IDENTIFIER);
		assertString(" ", buf);
		assertString(it.second->getName(), buf);
		assertString("\n", buf);
	}
	assertEOF(buf);
}

void StorageReplicatedMergeTree::createReplica()
{
	zookeeper.create(replica_path, "", zkutil::CreateMode::Persistent);
	zookeeper.create(replica_path + "/host", "", zkutil::CreateMode::Persistent);
	zookeeper.create(replica_path + "/log", "", zkutil::CreateMode::Persistent);
	zookeeper.create(replica_path + "/log_pointers", "", zkutil::CreateMode::Persistent);
	zookeeper.create(replica_path + "/queue", "", zkutil::CreateMode::Persistent);
	zookeeper.create(replica_path + "/parts", "", zkutil::CreateMode::Persistent);
}

void StorageReplicatedMergeTree::activateReplica()
{
	std::stringstream host;
	host << "host: " << context.getInterserverIOHost() << std::endl;
	host << "port: " << context.getInterserverIOPort() << std::endl;

	/// Одновременно объявим, что эта реплика активна, и обновим хост.
	zkutil::Ops ops;
	ops.push_back(new zkutil::Op::Create(replica_path + "/is_active", "", zookeeper.getDefaultACL(), zkutil::CreateMode::Ephemeral));
	ops.push_back(new zkutil::Op::SetData(replica_path + "/host", host.str(), -1));

	try
	{
		zookeeper.multi(ops);
	}
	catch (zkutil::KeeperException & e)
	{
		if (e.code == zkutil::ReturnCode::NodeExists)
			throw Exception("Replica " + replica_path + " appears to be already active. If you're sure it's not, "
				"try again in a minute or remove znode " + replica_path + "/is_active manually", ErrorCodes::REPLICA_IS_ALREADY_ACTIVE);

		throw;
	}

	replica_is_active_node = zkutil::EphemeralNodeHolder::existing(replica_path + "/is_active", zookeeper);
}

bool StorageReplicatedMergeTree::isTableEmpty()
{
	Strings replicas = zookeeper.getChildren(zookeeper_path + "/replicas");
	for (const auto & replica : replicas)
	{
		if (!zookeeper.getChildren(zookeeper_path + "/replicas/" + replica + "/parts").empty())
			return false;
	}
	return true;
}

void StorageReplicatedMergeTree::checkParts()
{
	Strings expected_parts_vec = zookeeper.getChildren(replica_path + "/parts");
	NameSet expected_parts(expected_parts_vec.begin(), expected_parts_vec.end());

	MergeTreeData::DataParts parts = data.getDataParts();

	MergeTreeData::DataPartsVector unexpected_parts;
	for (const auto & part : parts)
	{
		if (expected_parts.count(part->name))
		{
			expected_parts.erase(part->name);
		}
		else
		{
			unexpected_parts.push_back(part);
		}
	}

	if (!expected_parts.empty())
		throw Exception("Not found " + toString(expected_parts.size())
			+ " parts (including " + *expected_parts.begin() + ") in table " + data.getTableName(),
			ErrorCodes::NOT_FOUND_EXPECTED_DATA_PART);

	if (unexpected_parts.size() > 1)
		throw Exception("More than one unexpected part (including " + unexpected_parts[0]->name
			+ ") in table " + data.getTableName(),
			ErrorCodes::TOO_MANY_UNEXPECTED_DATA_PARTS);

	for (MergeTreeData::DataPartPtr part : unexpected_parts)
	{
		LOG_ERROR(log, "Unexpected part " << part->name << ". Renaming it to ignored_" + part->name);
		data.renameAndDetachPart(part, "ignored_");
	}
}

void StorageReplicatedMergeTree::loadQueue()
{
	Poco::ScopedLock<Poco::FastMutex> lock(queue_mutex);

	Strings children = zookeeper.getChildren(replica_path + "/queue");
	std::sort(children.begin(), children.end());
	for (const String & child : children)
	{
		String s = zookeeper.get(replica_path + "/queue/" + child);
		LogEntry entry = LogEntry::parse(s);
		entry.znode_name = child;
		entry.tagPartsAsCurrentlyMerging(*this);
		queue.push_back(entry);
	}
}

void StorageReplicatedMergeTree::pullLogsToQueue()
{
	Poco::ScopedLock<Poco::FastMutex> lock(queue_mutex);

	/// Сольем все логи в хронологическом порядке.

	struct LogIterator
	{
		String replica;		/// Имя реплики.
		UInt64 index;		/// Номер записи в логе (суффикс имени ноды).

		Int64 timestamp;	/// Время (czxid) создания записи в логе.
		String entry_str;	/// Сама запись.

		bool operator<(const LogIterator & rhs) const
		{
			return timestamp < rhs.timestamp;
		}

		bool readEntry(zkutil::ZooKeeper & zookeeper, const String & zookeeper_path)
		{
			String index_str = toString(index);
			while (index_str.size() < 10)
				index_str = '0' + index_str;
			zkutil::Stat stat;
			if (!zookeeper.tryGet(zookeeper_path + "/replicas/" + replica + "/log/log-" + index_str, entry_str, &stat))
				return false;
			timestamp = stat.getczxid();
			return true;
		}
	};

	typedef std::priority_queue<LogIterator> PriorityQueue;
	PriorityQueue priority_queue;

	Strings replicas = zookeeper.getChildren(zookeeper_path + "/replicas");

	for (const String & replica : replicas)
	{
		String index_str;
		UInt64 index;

		if (zookeeper.tryGet(replica_path + "/log_pointers/" + replica, index_str))
		{
			index = parse<UInt64>(index_str);
		}
		else
		{
			/// Если у нас еще нет указателя на лог этой реплики, поставим указатель на первую запись в нем.
			Strings entries = zookeeper.getChildren(zookeeper_path + "/replicas/" + replica + "/log");
			std::sort(entries.begin(), entries.end());
			index = entries.empty() ? 0 : parse<UInt64>(entries[0].substr(strlen("log-")));

			zookeeper.create(replica_path + "/log_pointers/" + replica, toString(index), zkutil::CreateMode::Persistent);
		}

		LogIterator iterator;
		iterator.replica = replica;
		iterator.index = index;

		if (iterator.readEntry(zookeeper, zookeeper_path))
			priority_queue.push(iterator);
	}

	size_t count = 0;

	while (!priority_queue.empty())
	{
		LogIterator iterator = priority_queue.top();
		priority_queue.pop();
		++count;

		LogEntry entry = LogEntry::parse(iterator.entry_str);

		/// Одновременно добавим запись в очередь и продвинем указатель на лог.
		zkutil::Ops ops;
		ops.push_back(new zkutil::Op::Create(
			replica_path + "/queue/queue-", iterator.entry_str, zookeeper.getDefaultACL(), zkutil::CreateMode::PersistentSequential));
		ops.push_back(new zkutil::Op::SetData(
			replica_path + "/log_pointers/" + iterator.replica, toString(iterator.index + 1), -1));
		auto results = zookeeper.multi(ops);

		String path_created = dynamic_cast<zkutil::OpResult::Create &>((*results)[0]).getPathCreated();
		entry.znode_name = path_created.substr(path_created.find_last_of('/') + 1);
		entry.tagPartsAsCurrentlyMerging(*this);
		queue.push_back(entry);

		++iterator.index;
		if (iterator.readEntry(zookeeper, zookeeper_path))
			priority_queue.push(iterator);
	}

	if (count > 0)
		LOG_DEBUG(log, "Pulled " << count << " entries to queue");
}

bool StorageReplicatedMergeTree::shouldExecuteLogEntry(const LogEntry & entry)
{
	if (entry.type == LogEntry::MERGE_PARTS)
	{
		/** Если какая-то из нужных частей сейчас передается или мерджится, подождем окончания этой операции.
		  * Иначе, даже если всех нужных частей для мерджа нет, нужно попытаться сделать мердж.
		  * Если каких-то частей не хватает, вместо мерджа будет попытка скачать кусок.
		  * Такая ситуация возможна, если получение какого-то куска пофейлилось, и его переместили в конец очереди.
		  */
		for (const auto & name : entry.parts_to_merge)
		{
			if (future_parts.count(name))
			{
				LOG_TRACE(log, "Not merging into part " << entry.new_part_name << " yet because part " << name << " is not ready yet.");
				return false;
			}
		}
	}

	return true;
}

void StorageReplicatedMergeTree::executeLogEntry(const LogEntry & entry)
{
	if (entry.type == LogEntry::GET_PART ||
		entry.type == LogEntry::MERGE_PARTS)
	{
		/// Если у нас уже есть этот кусок или покрывающий его кусок, ничего делать не нужно.
		MergeTreeData::DataPartPtr containing_part = data.getContainingPart(entry.new_part_name);

		/// Даже если кусок есть локально, его (в исключительных случаях) может не быть в zookeeper.
		if (containing_part && zookeeper.exists(replica_path + "/parts/" + containing_part->name))
		{
			if (!(entry.type == LogEntry::GET_PART && entry.source_replica == replica_name))
				LOG_DEBUG(log, "Skipping action for part " + entry.new_part_name + " - part already exists");
			return;
		}
	}

	if (entry.type == LogEntry::GET_PART && entry.source_replica == replica_name)
		LOG_ERROR(log, "Part " << entry.new_part_name << " from own log doesn't exist. This is a bug.");

	bool do_fetch = false;

	if (entry.type == LogEntry::GET_PART)
	{
		do_fetch = true;
	}
	else if (entry.type == LogEntry::MERGE_PARTS)
	{
		MergeTreeData::DataPartsVector parts;
		bool have_all_parts = true;;
		for (const String & name : entry.parts_to_merge)
		{
			MergeTreeData::DataPartPtr part = data.getContainingPart(name);
			if (!part)
			{
				have_all_parts = false;
				break;
			}
			if (part->name != name)
			{
				LOG_ERROR(log, "Log and parts set look inconsistent: " << name << " is covered by " << part->name
					<< " but should be merged into " << entry.new_part_name);
				have_all_parts = false;
				break;
			}
			parts.push_back(part);
		}

		if (!have_all_parts)
		{
			/// Если нет всех нужных кусков, попробуем взять у кого-нибудь уже помердженный кусок.
			do_fetch = true;
			LOG_DEBUG(log, "Don't have all parts for merge " << entry.new_part_name << "; will try to fetch it instead");
		}
		else
		{
			MergeTreeData::DataPartPtr part = merger.mergeParts(parts, entry.new_part_name);

			zkutil::Ops ops;
			ops.push_back(new zkutil::Op::Create(
				replica_path + "/parts/" + part->name,
				"",
				zookeeper.getDefaultACL(),
				zkutil::CreateMode::Persistent));
			ops.push_back(new zkutil::Op::Create(
				replica_path + "/parts/" + part->name + "/checksums",
				part->checksums.toString(),
				zookeeper.getDefaultACL(),
				zkutil::CreateMode::Persistent));

			for (const auto & part : parts)
			{
				ops.push_back(new zkutil::Op::Remove(replica_path + "/parts/" + part->name + "/checksums", -1));
				ops.push_back(new zkutil::Op::Remove(replica_path + "/parts/" + part->name, -1));
			}

			zookeeper.multi(ops);

			parts.clear();
			data.clearOldParts();

			ProfileEvents::increment(ProfileEvents::ReplicatedPartMerges);
		}
	}
	else
	{
		throw Exception("Unexpected log entry type: " + toString(static_cast<int>(entry.type)));
	}

	if (do_fetch)
	{
		try
		{
			String replica = findActiveReplicaHavingPart(entry.new_part_name);
			fetchPart(entry.new_part_name, replica);

			if (entry.type == LogEntry::MERGE_PARTS)
				ProfileEvents::increment(ProfileEvents::ReplicatedPartFetchesOfMerged);
		}
		catch (...)
		{
			/** Если не получилось скачать кусок, нужный для какого-то мерджа, лучше не пытаться получить другие куски для этого мерджа,
			  * а попытаться сразу получить помердженный кусок. Чтобы так получилось, переместим действия для получения остальных кусков
			  * для этого мерджа в конец очереди.
			  *
			  */
			try
			{
				Poco::ScopedLock<Poco::FastMutex> lock(queue_mutex);

				/// Найдем действие по объединению этого куска с другими. Запомним других.
				StringSet parts_for_merge;
				LogEntries::iterator merge_entry;
				for (LogEntries::iterator it = queue.begin(); it != queue.end(); ++it)
				{
					if (it->type == LogEntry::MERGE_PARTS)
					{
						if (std::find(it->parts_to_merge.begin(), it->parts_to_merge.end(), entry.new_part_name)
							!= it->parts_to_merge.end())
						{
							parts_for_merge = StringSet(it->parts_to_merge.begin(), it->parts_to_merge.end());
							merge_entry = it;
							break;
						}
					}
				}

				if (!parts_for_merge.empty())
				{
					/// Переместим в конец очереди действия, получающие parts_for_merge.
					for (LogEntries::iterator it = queue.begin(); it != queue.end();)
					{
						auto it0 = it;
						++it;

						if (it0 == merge_entry)
							break;

						if ((it0->type == LogEntry::MERGE_PARTS || it0->type == LogEntry::GET_PART)
							&& parts_for_merge.count(it0->new_part_name))
						{
							queue.splice(queue.end(), queue, it0, it);
						}
					}
				}
			}
			catch (...)
			{
				tryLogCurrentException(__PRETTY_FUNCTION__);
			}

			throw;
		}
	}
}

void StorageReplicatedMergeTree::queueUpdatingThread()
{
	while (!shutdown_called)
	{
		try
		{
			pullLogsToQueue();
		}
		catch (...)
		{
			tryLogCurrentException(__PRETTY_FUNCTION__);
		}

		std::this_thread::sleep_for(QUEUE_UPDATE_SLEEP);
	}
}

void StorageReplicatedMergeTree::queueThread()
{
	while (!shutdown_called)
	{
		LogEntry entry;
		bool have_work = false;

		try
		{
			Poco::ScopedLock<Poco::FastMutex> lock(queue_mutex);
			bool empty = queue.empty();
			if (!empty)
			{
				for (LogEntries::iterator it = queue.begin(); it != queue.end(); ++it)
				{
					if (shouldExecuteLogEntry(*it))
					{
						entry = *it;
						entry.tagPartAsFuture(*this);
						queue.erase(it);
						have_work = true;
						break;
					}
				}
			}
		}
		catch (...)
		{
			tryLogCurrentException(__PRETTY_FUNCTION__);
		}

		if (!have_work)
		{
			std::this_thread::sleep_for(QUEUE_NO_WORK_SLEEP);
			continue;
		}

		bool success = false;

		try
		{
			executeLogEntry(entry);

			auto code = zookeeper.tryRemove(replica_path + "/queue/" + entry.znode_name);
			if (code != zkutil::ReturnCode::Ok)
				LOG_ERROR(log, "Couldn't remove " << replica_path + "/queue/" + entry.znode_name << ": "
					<< zkutil::ReturnCode::toString(code) + ". There must be a bug somewhere. Ignoring it.");

			success = true;
		}
		catch (Exception & e)
		{
			if (e.code() == ErrorCodes::NO_REPLICA_HAS_PART)
				/// Если ни у кого нет нужного куска, это нормальная ситуация; не будем писать в лог с уровнем Error.
				LOG_INFO(log, e.displayText());
			else
				tryLogCurrentException(__PRETTY_FUNCTION__);
		}
		catch (...)
		{
			tryLogCurrentException(__PRETTY_FUNCTION__);
		}

		if (shutdown_called)
			break;

		if (success)
		{
			entry.currently_merging_tagger = nullptr;
			std::this_thread::sleep_for(QUEUE_AFTER_WORK_SLEEP);
		}
		else
		{
			{
				/// Добавим действие, которое не получилось выполнить, в конец очереди.
				entry.future_part_tagger = nullptr;
				Poco::ScopedLock<Poco::FastMutex> lock(queue_mutex);
				queue.push_back(entry);
			}
			entry.currently_merging_tagger = nullptr;
			std::this_thread::sleep_for(QUEUE_ERROR_SLEEP);
		}
	}
}

void StorageReplicatedMergeTree::mergeSelectingThread()
{
	pullLogsToQueue();

	while (!shutdown_called && is_leader_node)
	{
		bool success = false;

		try
		{
			size_t merges_queued = 0;

			{
				Poco::ScopedLock<Poco::FastMutex> lock(queue_mutex);

				for (const auto & entry : queue)
					if (entry.type == LogEntry::MERGE_PARTS)
						++merges_queued;
			}

			if (merges_queued >= data.settings.merging_threads)
			{
				std::this_thread::sleep_for(MERGE_SELECTING_SLEEP);
				continue;
			}

			/// Есть ли активный мердж крупных кусков.
			bool has_big_merge = false;

			{
				Poco::ScopedLock<Poco::FastMutex> lock(currently_merging_mutex);

				for (const auto & name : currently_merging)
				{
					MergeTreeData::DataPartPtr part = data.getContainingPart(name);
					if (!part)
						continue;
					if (part->name != name)
					{
						LOG_INFO(log, "currently_merging contains obsolete part " << name << " contained in" << part->name);
						continue;
					}
					if (part->size * data.index_granularity > 25 * 1024 * 1024)
					{
						has_big_merge = true;
						break;
					}
				}
			}

			MergeTreeData::DataPartsVector parts;

			{
				Poco::ScopedLock<Poco::FastMutex> lock(currently_merging_mutex);

				String merged_name;
				auto can_merge = std::bind(
					&StorageReplicatedMergeTree::canMergeParts, this, std::placeholders::_1, std::placeholders::_2);

				LOG_TRACE(log, "Selecting parts to merge" << (has_big_merge ? " (only small)" : ""));

				if (merger.selectPartsToMerge(parts, merged_name, 0, false, false, has_big_merge, can_merge) ||
					merger.selectPartsToMerge(parts, merged_name, 0,  true, false, has_big_merge, can_merge))
				{
					LogEntry entry;
					entry.type = LogEntry::MERGE_PARTS;
					entry.source_replica = replica_name;
					entry.new_part_name = merged_name;

					for (const auto & part : parts)
					{
						entry.parts_to_merge.push_back(part->name);
					}

					zookeeper.create(replica_path + "/log/log-", entry.toString(), zkutil::CreateMode::PersistentSequential);

					success = true;
				}
			}

			/// Нужно загрузить новую запись в очередь перед тем, как в следующий раз выбирать куски для слияния.
			///  (чтобы куски пометились как currently_merging).
			pullLogsToQueue();

			for (size_t i = 0; i + 1 < parts.size(); ++i)
			{
				/// Уберем больше не нужные отметки о несуществующих блоках.
				for (UInt64 number = parts[i]->right + 1; number <= parts[i + 1]->left - 1; ++number)
				{
					String number_str = toString(number);
					while (number_str.size() < 10)
						number_str = '0' + number_str;
					String path = zookeeper_path + "/block_numbers/block-" + number_str;

					zookeeper.tryRemove(path);
				}
			}
		}
		catch (...)
		{
			tryLogCurrentException(__PRETTY_FUNCTION__);
		}

		if (shutdown_called)
			break;

		if (!success)
			std::this_thread::sleep_for(MERGE_SELECTING_SLEEP);
	}
}

bool StorageReplicatedMergeTree::canMergeParts(const MergeTreeData::DataPartPtr & left, const MergeTreeData::DataPartPtr & right)
{
	if (currently_merging.count(left->name) || currently_merging.count(right->name))
		return false;

	/// Можно слить куски, если все номера между ними заброшены - не соответствуют никаким блокам.
	for (UInt64 number = left->right + 1; number <= right->left - 1; ++number)
	{
		String number_str = toString(number);
		while (number_str.size() < 10)
			number_str = '0' + number_str;
		String path = zookeeper_path + "/block_numbers/block-" + number_str;

		if (AbandonableLockInZooKeeper::check(path, zookeeper) != AbandonableLockInZooKeeper::ABANDONED)
		{
			LOG_DEBUG(log, "Can't merge parts " << left->name << " and " << right->name << " because block " << path << " exists");
			return false;
		}
	}

	return true;
}

void StorageReplicatedMergeTree::becomeLeader()
{
	LOG_INFO(log, "Became leader");
	is_leader_node = true;
	merge_selecting_thread = std::thread(&StorageReplicatedMergeTree::mergeSelectingThread, this);
}

String StorageReplicatedMergeTree::findActiveReplicaHavingPart(const String & part_name)
{
	Strings replicas = zookeeper.getChildren(zookeeper_path + "/replicas");

	/// Из реплик, у которых есть кусок, выберем одну равновероятно.
	std::random_shuffle(replicas.begin(), replicas.end());

	for (const String & replica : replicas)
	{
		if (zookeeper.exists(zookeeper_path + "/replicas/" + replica + "/parts/" + part_name) &&
			zookeeper.exists(zookeeper_path + "/replicas/" + replica + "/is_active"))
			return replica;
	}

	throw Exception("No active replica has part " + part_name, ErrorCodes::NO_REPLICA_HAS_PART);
}

void StorageReplicatedMergeTree::fetchPart(const String & part_name, const String & replica_name)
{
	LOG_DEBUG(log, "Fetching part " << part_name << " from " << replica_name);

	auto table_lock = lockStructure(true);

	String host;
	int port;

	String host_port_str = zookeeper.get(zookeeper_path + "/replicas/" + replica_name + "/host");
	ReadBufferFromString buf(host_port_str);
	assertString("host: ", buf);
	readString(host, buf);
	assertString("\nport: ", buf);
	readText(port, buf);
	assertString("\n", buf);
	assertEOF(buf);

	MergeTreeData::MutableDataPartPtr part = fetcher.fetchPart(part_name, zookeeper_path + "/replicas/" + replica_name, host, port);
	auto removed_parts = data.renameTempPartAndReplace(part);

	zkutil::Ops ops;
	ops.push_back(new zkutil::Op::Create(
		replica_path + "/parts/" + part->name,
		"",
		zookeeper.getDefaultACL(),
		zkutil::CreateMode::Persistent));
	ops.push_back(new zkutil::Op::Create(
		replica_path + "/parts/" + part->name + "/checksums",
		part->checksums.toString(),
		zookeeper.getDefaultACL(),
		zkutil::CreateMode::Persistent));

	for (const auto & removed_part : removed_parts)
	{
		LOG_DEBUG(log, "Part " << removed_part->name << " is rendered obsolete by fetching part " << part_name);
		ProfileEvents::increment(ProfileEvents::ObsoleteReplicatedParts);

		ops.push_back(new zkutil::Op::Remove(replica_path + "/parts/" + removed_part->name + "/checksums", -1));
		ops.push_back(new zkutil::Op::Remove(replica_path + "/parts/" + removed_part->name, -1));
	}

	zookeeper.multi(ops);

	ProfileEvents::increment(ProfileEvents::ReplicatedPartFetches);

	LOG_DEBUG(log, "Fetched part");
}

void StorageReplicatedMergeTree::shutdown()
{
	if (shutdown_called)
		return;
	leader_election = nullptr;
	shutdown_called = true;
	replica_is_active_node = nullptr;
	endpoint_holder = nullptr;

	LOG_TRACE(log, "Waiting for threads to finish");
	if (is_leader_node)
		merge_selecting_thread.join();
	queue_updating_thread.join();
	for (auto & thread : queue_threads)
		thread.join();
	LOG_TRACE(log, "Threads finished");
}

StorageReplicatedMergeTree::~StorageReplicatedMergeTree()
{
	try
	{
		shutdown();
	}
	catch(...)
	{
		tryLogCurrentException("~StorageReplicatedMergeTree");
	}
}

BlockInputStreams StorageReplicatedMergeTree::read(
		const Names & column_names,
		ASTPtr query,
		const Settings & settings,
		QueryProcessingStage::Enum & processed_stage,
		size_t max_block_size,
		unsigned threads)
{
	return reader.read(column_names, query, settings, processed_stage, max_block_size, threads);
}

BlockOutputStreamPtr StorageReplicatedMergeTree::write(ASTPtr query)
{
	String insert_id;
	if (ASTInsertQuery * insert = dynamic_cast<ASTInsertQuery *>(&*query))
		insert_id = insert->insert_id;

	return new ReplicatedMergeTreeBlockOutputStream(*this, insert_id);
}

void StorageReplicatedMergeTree::drop()
{
	shutdown();

	replica_is_active_node = nullptr;
	zookeeper.removeRecursive(replica_path);
	if (zookeeper.getChildren(zookeeper_path + "/replicas").empty())
		zookeeper.removeRecursive(zookeeper_path);
}

void StorageReplicatedMergeTree::LogEntry::writeText(WriteBuffer & out) const
{
	writeString("format version: 1\n", out);
	writeString("source replica: ", out);
	writeString(source_replica, out);
	writeString("\n", out);
	switch (type)
	{
		case GET_PART:
			writeString("get\n", out);
			writeString(new_part_name, out);
			break;
		case MERGE_PARTS:
			writeString("merge\n", out);
			for (const String & s : parts_to_merge)
			{
				writeString(s, out);
				writeString("\n", out);
			}
			writeString("into\n", out);
			writeString(new_part_name, out);
			break;
	}
	writeString("\n", out);
}

void StorageReplicatedMergeTree::LogEntry::readText(ReadBuffer & in)
{
	String type_str;

	assertString("format version: 1\n", in);
	assertString("source replica: ", in);
	readString(source_replica, in);
	assertString("\n", in);
	readString(type_str, in);
	assertString("\n", in);

	if (type_str == "get")
	{
		type = GET_PART;
		readString(new_part_name, in);
	}
	else if (type_str == "merge")
	{
		type = MERGE_PARTS;
		while (true)
		{
			String s;
			readString(s, in);
			assertString("\n", in);
			if (s == "into")
				break;
			parts_to_merge.push_back(s);
		}
		readString(new_part_name, in);
	}
	assertString("\n", in);
}

}