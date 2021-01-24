//
// Created by leipeng on 2021/1/21.
//

#include <utilities/json/json_plugin_repo.h>
#include <utilities/json/json_plugin_factory.h>
#include <db/compaction/compaction_executor.h>
#include <db/error_handler.h>
#include <rocksdb/merge_operator.h>
#include <env/composite_env_wrapper.h>
#include <terark/fstring.hpp>
//#include <terark/io/FileStream.hpp>
//#include <terark/io/DataIO.hpp>

namespace ROCKSDB_NAMESPACE {

//using namespace terark;
using namespace std;

// for CompactionFilter:
// bind extra to CompactionFilterFactory, so the CompactionFilter created
// by CompactionFilterFactory should return its internal data such as some
// statistics into the ResultType object, then the ResultType object will be
// returned to the rocksdb process.
// 1. For results->sub_compacts.field
//    CompactionFilterFactory should use GetSubCompactIdx() as index to get
//    per-thread(per-SubCompact) ResultType
//    to 'extra' (type ResultType).
//    really bind to factory's product object
// 2. For results->field, this is an aggregated info, bind to factory itself.
template<class Object, class ResultType>
void ExtraBind(Object* obj, ResultType* extra) {
  const string& clazz = obj->Name();
  const ExtraBinderFunc<Object, ResultType>* func =
      ExtraBinder<Object, ResultType>::NullablePlugin(clazz);
  if (func) {
    func->Bind(obj, extra);
  }
}

template<class Ptr, class ResultType>
void CreatePluginTpl(Ptr& ptr, const ObjectRpcParam& param,
                     vector<ResultType>* sub_compact_result,
                     string* aggregated_result) {
  if (param.clazz.empty()) {
    return;  // not defined
  }
  JsonPluginRepo repo; // empty repo
  json cons_params = json::parse(param.params);
  ptr = PluginFactory<Ptr>::AcquirePlugin(param.clazz, cons_params, repo);
  if (!param.serde.empty())
    SerDe_DeSerialize(param.clazz, param.serde, &*ptr);
  ExtraBind(&*ptr, sub_compact_result);
  ExtraBind(&*ptr, aggregated_result);
}

const char* GetEnvString(const char* name, const char* Default = nullptr) {
  if (auto value = getenv(name)) {
    return value;
  }
  else if (nullptr == Default) {
    THROW_STD(invalid_argument, "missing env var: %s", name);
  }
}

string GetDirFromEnv(const char* name, const char* Default = nullptr) {
  string dir = GetEnvString(name, Default);
  if (!dir.empty() && '/' != dir.back()) {
    dir.push_back('/');
  }
  return dir;
}

class Main {
// used for mapping hoster node dir to worker node dir
const string g_worker_root = GetDirFromEnv("WORKER_ROOT");
const string g_hoster_root = GetDirFromEnv("HOSTER_ROOT");

string GetWorkerNodeDir(const string& hostNodeDir) {
  if (Slice(hostNodeDir).starts_with(g_hoster_root)) {
    string dir;
    size_t suffixLen = hostNodeDir.size() - g_hoster_root.size();
    dir.reserve(g_worker_root.size() + suffixLen);
    dir.append(g_worker_root);
    dir.append(hostNodeDir.data() + g_hoster_root.size(), suffixLen);
  }
  THROW_STD(invalid_argument,
            "hostNodeDir = '%s' does not start with HOSTER_ROOT='%s'",
            hostNodeDir.c_str(), g_hoster_root.c_str());
}
string MakeOutputPath(const CompactionParams& params) {
  string path = GetWorkerNodeDir(params.cf_paths[0].path);
  char buf[128];
  path.append(buf, snprintf(buf, sizeof(buf), "/job-%08d", params.job_id));
  return path;
}
public:
int main(int argc, char* argv[]) {
  SetCompactionWorker(true);
  unique_ptr<CompactionResults> results(new CompactionResults());
  TERARK_UNUSED_VAR(argc);
  TERARK_UNUSED_VAR(argv);
try {
/*
  string json_file;
  if (auto env = getenv("JSON_DB_CONFIG")) {
    json_file = env;
  }
  else {
    fprintf(stderr, "");
    return 1;
  }

  JsonPluginRepo repo;
  {
    Status s = repo.ImportJsonFile(json_file);
    if (!s.ok()) {
      fprintf(stderr, "ERROR: %s\n", s.ToString().c_str());
      return 1;
    }
  }
*/
  CompactionParams  params;
  SerDeRead(stdin, &params);
  EnvOptions env_options; env_options.use_mmap_reads = true;
  Env* env = Env::Default();
  shared_ptr<FileSystem> fs(make_shared<LegacyFileSystemWrapper>(env));
  shared_ptr<Cache> table_cache = NewLRUCache(50000, 16);
  WriteController write_controller;
  WriteBufferManager write_buffer_manager(params.db_write_buffer_size);
  ImmutableDBOptions imm_dbo;
  MutableDBOptions   mut_dbo;
  ColumnFamilyOptions cfo;
  results->sub_compacts.resize(params.max_subcompactions);

#define MyCreatePlugin2(obj, field1, field2) \
  CreatePluginTpl(obj.field1, params.field2, &results->sub_compacts.field2, &results->field2)
#define MyCreatePlugin1(obj, field) MyCreatePlugin2(obj, field, field)
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  MyCreatePlugin1(cfo, compaction_filter_factory);
  MyCreatePlugin2(cfo, comparator, user_comparator);
  MyCreatePlugin1(cfo, merge_operator);
  MyCreatePlugin1(cfo, table_factory);
  MyCreatePlugin1(cfo, prefix_extractor);
  MyCreatePlugin1(cfo, sst_partitioner_factory);

  string output_dir = MakeOutputPath(params);
  imm_dbo.env = env;
  imm_dbo.fs = fs;
  imm_dbo.db_paths.clear();
  imm_dbo.db_paths.reserve(params.cf_paths.size() + 1);
  for (auto& dir : params.cf_paths) {
    imm_dbo.db_paths.emplace_back(GetWorkerNodeDir(dir.path), dir.target_size);
  }
  imm_dbo.db_paths.emplace_back(output_dir, UINT64_MAX);
  cfo.cf_paths = imm_dbo.db_paths;
  string fake_dbname = output_dir;
  env->CreateDirIfMissing(fake_dbname);
  unique_ptr<VersionSet> versions(
      new VersionSet(fake_dbname, &imm_dbo, env_options, table_cache.get(),
                     &write_buffer_manager, &write_controller,
                     /*block_cache_tracer=*/nullptr, /*io_tracer=*/nullptr));
  params.version_set.To(versions.get());

  uint64_t log_number = 0;
  VersionEdit new_db;
  new_db.SetLogNumber(log_number);
  new_db.SetNextFile(params.version_set.next_file_number);
  new_db.SetLastSequence(params.version_set.last_sequence);
  new_db.SetColumnFamily(params.cf_id);
  new_db.AddColumnFamily(params.cf_name);
  for (auto& onelevel : *params.inputs) {
    for (auto& file : onelevel.files) {
      new_db.AddFile(onelevel.level, *file); // file will be copied
    }
  }
  auto manifest_fnum = params.version_set.manifest_file_number;
  const string manifest = DescriptorFileName(fake_dbname, manifest_fnum);
  {
    unique_ptr<WritableFile> file;
    Status s1 = env->NewWritableFile(
        manifest, &file, env->OptimizeForManifestWrite(env_options));
    TERARK_VERIFY_F(s1.ok(), "%s", s1.ToString().c_str());
    unique_ptr<WritableFileWriter> file_writer(new WritableFileWriter(
        NewLegacyWritableFileWrapper(std::move(file)), manifest, env_options));
    log::Writer log(std::move(file_writer), log_number, false);
    string record;
    new_db.EncodeTo(&record);
    auto s2 = log.AddRecord(record);
    TERARK_VERIFY_F(s2.ok(), "%s", s2.ToString().c_str());
  // Make "CURRENT" file that points to the new manifest file.
    auto s3 = SetCurrentFile(fs.get(), fake_dbname, manifest_fnum, nullptr);
    TERARK_VERIFY_F(s3.ok(), "%s", s3.ToString().c_str());
    std::vector<ColumnFamilyDescriptor> column_families;
    column_families.emplace_back(params.cf_name, cfo);
    auto s4 = versions->Recover(column_families, false);
    TERARK_VERIFY_F(s4.ok(), "%s", s4.ToString().c_str());
  }
  auto cfd = versions->GetColumnFamilySet()->GetColumnFamily(params.cf_id);
  size_t output_path_id = params.cf_paths.size();
  VersionStorageInfo* storage_info = cfd->current()->storage_info();
  vector<CompactionInputFiles> inputs = *params.inputs;
  for (auto& onelevel : inputs) {
    onelevel.files = storage_info->LevelFiles(onelevel.level);
  }
  Compaction compaction(storage_info,
      *cfd->ioptions(), *cfd->GetLatestMutableCFOptions(), mut_dbo, inputs,
      params.output_level, params.target_file_size, params.max_compaction_bytes,
      uint32_t(output_path_id), params.compression, params.compression_opts,
      params.max_subcompactions, *params.grandparents, params.manual_compaction,
      params.score, params.deletion_compaction, params.compaction_reason);
  compaction.SetInputVersion(cfd->current());
//----------------------------------------------------------------------------
  LogBuffer log_buffer(params.compaction_log_level, imm_dbo.info_log.get());
  InstrumentedMutex mutex;
  atomic<bool> shutting_down(false);
  auto statistics = CreateDBStatistics();
  ErrorHandler error_handler(nullptr, imm_dbo, &mutex);
  mutex.Lock();
  EventLogger event_logger(imm_dbo.info_log.get());
  SnapshotChecker* snapshot_checker = nullptr;
  if (!params.full_history_ts_low.empty()) {
    TERARK_VERIFY_EQ(cfo.comparator->timestamp_size(), params.full_history_ts_low.size());
  }
  CompactionJob compaction_job(
      params.job_id, &compaction, imm_dbo, env_options, versions.get(),
      &shutting_down, params.preserve_deletes_seqnum, &log_buffer,
      nullptr, nullptr, nullptr, nullptr,
      &mutex, &error_handler, *params.existing_snapshots,
      params.earliest_write_conflict_snapshot, snapshot_checker,
      table_cache, &event_logger, params.paranoid_file_checks,
      false/*measure_io_stats*/,
      fake_dbname, &results->job_stats, Env::Priority::USER,
      nullptr /* IOTracer */,
      /*manual_compaction_paused=*/nullptr,
      params.db_id, params.db_session_id,
      params.full_history_ts_low);
  //VerifyInitializationOfCompactionJobStats(compaction_job_stats_);

  compaction_job.Prepare();
  mutex.Unlock();
  {
    Status s1 = compaction_job.Run();
    //TERARK_VERIFY_F(s1.ok(), "%s", s1.ToString().c_str());
    results->status = s1;
    //auto s2 = compaction_job.io_status();
    //TERARK_VERIFY_F(s2.ok(), "%s", s2.ToString().c_str());
  }
  SerDeWrite(stdout, results.get());
  return 0;
}
catch (const std::exception& ex) {
  SerDeWrite(stdout, results.get());
  fprintf(stderr, "%s:%d: %s: caught exception: %s\n",
          __FILE__, __LINE__, ROCKSDB_FUNC, ex.what());
  return 1;
}
catch (const ROCKSDB_NAMESPACE::Status& s) {
  SerDeWrite(stdout, results.get());
  fprintf(stderr, "%s:%d: %s: caught Status: %s\n",
          __FILE__, __LINE__, ROCKSDB_FUNC, s.ToString().c_str());
  return 1;
}

} // main

}; // class Main

} // namespace ROCKSDB_NAMESPACE

int main(int argc, char* argv[]) {
  ROCKSDB_NAMESPACE::Main m;
  return m.main(argc, argv);
}
