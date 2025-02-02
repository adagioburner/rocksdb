//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifdef GFLAGS
#include "db_stress_tool/db_stress_common.h"

namespace rocksdb {
class NonBatchedOpsStressTest : public StressTest {
 public:
  NonBatchedOpsStressTest() {}

  virtual ~NonBatchedOpsStressTest() {}

  virtual void VerifyDb(ThreadState* thread) const {
    ReadOptions options(FLAGS_verify_checksum, true);
    auto shared = thread->shared;
    const int64_t max_key = shared->GetMaxKey();
    const int64_t keys_per_thread = max_key / shared->GetNumThreads();
    int64_t start = keys_per_thread * thread->tid;
    int64_t end = start + keys_per_thread;
    uint64_t prefix_to_use =
        (FLAGS_prefix_size < 0) ? 1 : static_cast<size_t>(FLAGS_prefix_size);
    if (thread->tid == shared->GetNumThreads() - 1) {
      end = max_key;
    }
    for (size_t cf = 0; cf < column_families_.size(); ++cf) {
      if (thread->shared->HasVerificationFailedYet()) {
        break;
      }
      if (!thread->rand.OneIn(2)) {
        // Use iterator to verify this range
        std::unique_ptr<Iterator> iter(
            db_->NewIterator(options, column_families_[cf]));
        iter->Seek(Key(start));
        for (auto i = start; i < end; i++) {
          if (thread->shared->HasVerificationFailedYet()) {
            break;
          }
          // TODO(ljin): update "long" to uint64_t
          // Reseek when the prefix changes
          if (prefix_to_use > 0 &&
              i % (static_cast<int64_t>(1) << 8 * (8 - prefix_to_use)) == 0) {
            iter->Seek(Key(i));
          }
          std::string from_db;
          std::string keystr = Key(i);
          Slice k = keystr;
          Status s = iter->status();
          if (iter->Valid()) {
            if (iter->key().compare(k) > 0) {
              s = Status::NotFound(Slice());
            } else if (iter->key().compare(k) == 0) {
              from_db = iter->value().ToString();
              iter->Next();
            } else if (iter->key().compare(k) < 0) {
              VerificationAbort(shared, "An out of range key was found",
                                static_cast<int>(cf), i);
            }
          } else {
            // The iterator found no value for the key in question, so do not
            // move to the next item in the iterator
            s = Status::NotFound(Slice());
          }
          VerifyValue(static_cast<int>(cf), i, options, shared, from_db, s,
                      true);
          if (from_db.length()) {
            PrintKeyValue(static_cast<int>(cf), static_cast<uint32_t>(i),
                          from_db.data(), from_db.length());
          }
        }
      } else {
        // Use Get to verify this range
        for (auto i = start; i < end; i++) {
          if (thread->shared->HasVerificationFailedYet()) {
            break;
          }
          std::string from_db;
          std::string keystr = Key(i);
          Slice k = keystr;
          Status s = db_->Get(options, column_families_[cf], k, &from_db);
          VerifyValue(static_cast<int>(cf), i, options, shared, from_db, s,
                      true);
          if (from_db.length()) {
            PrintKeyValue(static_cast<int>(cf), static_cast<uint32_t>(i),
                          from_db.data(), from_db.length());
          }
        }
      }
    }
  }

  virtual void MaybeClearOneColumnFamily(ThreadState* thread) {
    if (FLAGS_clear_column_family_one_in != 0 && FLAGS_column_families > 1) {
      if (thread->rand.OneIn(FLAGS_clear_column_family_one_in)) {
        // drop column family and then create it again (can't drop default)
        int cf = thread->rand.Next() % (FLAGS_column_families - 1) + 1;
        std::string new_name = ToString(new_column_family_name_.fetch_add(1));
        {
          MutexLock l(thread->shared->GetMutex());
          fprintf(
              stdout,
              "[CF %d] Dropping and recreating column family. new name: %s\n",
              cf, new_name.c_str());
        }
        thread->shared->LockColumnFamily(cf);
        Status s = db_->DropColumnFamily(column_families_[cf]);
        delete column_families_[cf];
        if (!s.ok()) {
          fprintf(stderr, "dropping column family error: %s\n",
                  s.ToString().c_str());
          std::terminate();
        }
        s = db_->CreateColumnFamily(ColumnFamilyOptions(options_), new_name,
                                    &column_families_[cf]);
        column_family_names_[cf] = new_name;
        thread->shared->ClearColumnFamily(cf);
        if (!s.ok()) {
          fprintf(stderr, "creating column family error: %s\n",
                  s.ToString().c_str());
          std::terminate();
        }
        thread->shared->UnlockColumnFamily(cf);
      }
    }
  }

  virtual bool ShouldAcquireMutexOnKey() const { return true; }

  virtual Status TestGet(ThreadState* thread, const ReadOptions& read_opts,
                         const std::vector<int>& rand_column_families,
                         const std::vector<int64_t>& rand_keys) {
    auto cfh = column_families_[rand_column_families[0]];
    std::string key_str = Key(rand_keys[0]);
    Slice key = key_str;
    std::string from_db;
    Status s = db_->Get(read_opts, cfh, key, &from_db);
    if (s.ok()) {
      // found case
      thread->stats.AddGets(1, 1);
    } else if (s.IsNotFound()) {
      // not found case
      thread->stats.AddGets(1, 0);
    } else {
      // errors case
      thread->stats.AddErrors(1);
    }
    return s;
  }

  virtual std::vector<Status> TestMultiGet(
      ThreadState* thread, const ReadOptions& read_opts,
      const std::vector<int>& rand_column_families,
      const std::vector<int64_t>& rand_keys) {
    size_t num_keys = rand_keys.size();
    std::vector<std::string> key_str;
    std::vector<Slice> keys;
    key_str.reserve(num_keys);
    keys.reserve(num_keys);
    std::vector<PinnableSlice> values(num_keys);
    std::vector<Status> statuses(num_keys);
    ColumnFamilyHandle* cfh = column_families_[rand_column_families[0]];

    for (size_t i = 0; i < num_keys; ++i) {
      key_str.emplace_back(Key(rand_keys[i]));
      keys.emplace_back(key_str.back());
    }
    db_->MultiGet(read_opts, cfh, num_keys, keys.data(), values.data(),
                  statuses.data());
    for (const auto& s : statuses) {
      if (s.ok()) {
        // found case
        thread->stats.AddGets(1, 1);
      } else if (s.IsNotFound()) {
        // not found case
        thread->stats.AddGets(1, 0);
      } else {
        // errors case
        thread->stats.AddErrors(1);
      }
    }
    return statuses;
  }

  virtual Status TestPrefixScan(ThreadState* thread,
                                const ReadOptions& read_opts,
                                const std::vector<int>& rand_column_families,
                                const std::vector<int64_t>& rand_keys) {
    auto cfh = column_families_[rand_column_families[0]];
    std::string key_str = Key(rand_keys[0]);
    Slice key = key_str;
    Slice prefix = Slice(key.data(), FLAGS_prefix_size);

    std::string upper_bound;
    Slice ub_slice;
    ReadOptions ro_copy = read_opts;
    if (thread->rand.OneIn(2) && GetNextPrefix(prefix, &upper_bound)) {
      // For half of the time, set the upper bound to the next prefix
      ub_slice = Slice(upper_bound);
      ro_copy.iterate_upper_bound = &ub_slice;
    }

    Iterator* iter = db_->NewIterator(ro_copy, cfh);
    long count = 0;
    for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix);
         iter->Next()) {
      ++count;
    }
    assert(count <= (static_cast<long>(1) << ((8 - FLAGS_prefix_size) * 8)));
    Status s = iter->status();
    if (iter->status().ok()) {
      thread->stats.AddPrefixes(1, count);
    } else {
      thread->stats.AddErrors(1);
    }
    delete iter;
    return s;
  }

  virtual Status TestPut(ThreadState* thread, WriteOptions& write_opts,
                         const ReadOptions& read_opts,
                         const std::vector<int>& rand_column_families,
                         const std::vector<int64_t>& rand_keys,
                         char (&value)[100], std::unique_ptr<MutexLock>& lock) {
    auto shared = thread->shared;
    int64_t max_key = shared->GetMaxKey();
    int64_t rand_key = rand_keys[0];
    int rand_column_family = rand_column_families[0];
    while (!shared->AllowsOverwrite(rand_key) &&
           (FLAGS_use_merge || shared->Exists(rand_column_family, rand_key))) {
      lock.reset();
      rand_key = thread->rand.Next() % max_key;
      rand_column_family = thread->rand.Next() % FLAGS_column_families;
      lock.reset(
          new MutexLock(shared->GetMutexForKey(rand_column_family, rand_key)));
    }

    std::string key_str = Key(rand_key);
    Slice key = key_str;
    ColumnFamilyHandle* cfh = column_families_[rand_column_family];

    if (FLAGS_verify_before_write) {
      std::string key_str2 = Key(rand_key);
      Slice k = key_str2;
      std::string from_db;
      Status s = db_->Get(read_opts, cfh, k, &from_db);
      if (!VerifyValue(rand_column_family, rand_key, read_opts, shared, from_db,
                       s, true)) {
        return s;
      }
    }
    uint32_t value_base = thread->rand.Next() % shared->UNKNOWN_SENTINEL;
    size_t sz = GenerateValue(value_base, value, sizeof(value));
    Slice v(value, sz);
    shared->Put(rand_column_family, rand_key, value_base, true /* pending */);
    Status s;
    if (FLAGS_use_merge) {
      if (!FLAGS_use_txn) {
        s = db_->Merge(write_opts, cfh, key, v);
      } else {
#ifndef ROCKSDB_LITE
        Transaction* txn;
        s = NewTxn(write_opts, &txn);
        if (s.ok()) {
          s = txn->Merge(cfh, key, v);
          if (s.ok()) {
            s = CommitTxn(txn);
          }
        }
#endif
      }
    } else {
      if (!FLAGS_use_txn) {
        s = db_->Put(write_opts, cfh, key, v);
      } else {
#ifndef ROCKSDB_LITE
        Transaction* txn;
        s = NewTxn(write_opts, &txn);
        if (s.ok()) {
          s = txn->Put(cfh, key, v);
          if (s.ok()) {
            s = CommitTxn(txn);
          }
        }
#endif
      }
    }
    shared->Put(rand_column_family, rand_key, value_base, false /* pending */);
    if (!s.ok()) {
      fprintf(stderr, "put or merge error: %s\n", s.ToString().c_str());
      std::terminate();
    }
    thread->stats.AddBytesForWrites(1, sz);
    PrintKeyValue(rand_column_family, static_cast<uint32_t>(rand_key), value,
                  sz);
    return s;
  }

  virtual Status TestDelete(ThreadState* thread, WriteOptions& write_opts,
                            const std::vector<int>& rand_column_families,
                            const std::vector<int64_t>& rand_keys,
                            std::unique_ptr<MutexLock>& lock) {
    int64_t rand_key = rand_keys[0];
    int rand_column_family = rand_column_families[0];
    auto shared = thread->shared;
    int64_t max_key = shared->GetMaxKey();

    // OPERATION delete
    // If the chosen key does not allow overwrite and it does not exist,
    // choose another key.
    while (!shared->AllowsOverwrite(rand_key) &&
           !shared->Exists(rand_column_family, rand_key)) {
      lock.reset();
      rand_key = thread->rand.Next() % max_key;
      rand_column_family = thread->rand.Next() % FLAGS_column_families;
      lock.reset(
          new MutexLock(shared->GetMutexForKey(rand_column_family, rand_key)));
    }

    std::string key_str = Key(rand_key);
    Slice key = key_str;
    auto cfh = column_families_[rand_column_family];

    // Use delete if the key may be overwritten and a single deletion
    // otherwise.
    Status s;
    if (shared->AllowsOverwrite(rand_key)) {
      shared->Delete(rand_column_family, rand_key, true /* pending */);
      if (!FLAGS_use_txn) {
        s = db_->Delete(write_opts, cfh, key);
      } else {
#ifndef ROCKSDB_LITE
        Transaction* txn;
        s = NewTxn(write_opts, &txn);
        if (s.ok()) {
          s = txn->Delete(cfh, key);
          if (s.ok()) {
            s = CommitTxn(txn);
          }
        }
#endif
      }
      shared->Delete(rand_column_family, rand_key, false /* pending */);
      thread->stats.AddDeletes(1);
      if (!s.ok()) {
        fprintf(stderr, "delete error: %s\n", s.ToString().c_str());
        std::terminate();
      }
    } else {
      shared->SingleDelete(rand_column_family, rand_key, true /* pending */);
      if (!FLAGS_use_txn) {
        s = db_->SingleDelete(write_opts, cfh, key);
      } else {
#ifndef ROCKSDB_LITE
        Transaction* txn;
        s = NewTxn(write_opts, &txn);
        if (s.ok()) {
          s = txn->SingleDelete(cfh, key);
          if (s.ok()) {
            s = CommitTxn(txn);
          }
        }
#endif
      }
      shared->SingleDelete(rand_column_family, rand_key, false /* pending */);
      thread->stats.AddSingleDeletes(1);
      if (!s.ok()) {
        fprintf(stderr, "single delete error: %s\n", s.ToString().c_str());
        std::terminate();
      }
    }
    return s;
  }

  virtual Status TestDeleteRange(ThreadState* thread, WriteOptions& write_opts,
                                 const std::vector<int>& rand_column_families,
                                 const std::vector<int64_t>& rand_keys,
                                 std::unique_ptr<MutexLock>& lock) {
    // OPERATION delete range
    std::vector<std::unique_ptr<MutexLock>> range_locks;
    // delete range does not respect disallowed overwrites. the keys for
    // which overwrites are disallowed are randomly distributed so it
    // could be expensive to find a range where each key allows
    // overwrites.
    int64_t rand_key = rand_keys[0];
    int rand_column_family = rand_column_families[0];
    auto shared = thread->shared;
    int64_t max_key = shared->GetMaxKey();
    if (rand_key > max_key - FLAGS_range_deletion_width) {
      lock.reset();
      rand_key =
          thread->rand.Next() % (max_key - FLAGS_range_deletion_width + 1);
      range_locks.emplace_back(
          new MutexLock(shared->GetMutexForKey(rand_column_family, rand_key)));
    } else {
      range_locks.emplace_back(std::move(lock));
    }
    for (int j = 1; j < FLAGS_range_deletion_width; ++j) {
      if (((rand_key + j) & ((1 << FLAGS_log2_keys_per_lock) - 1)) == 0) {
        range_locks.emplace_back(new MutexLock(
            shared->GetMutexForKey(rand_column_family, rand_key + j)));
      }
    }
    shared->DeleteRange(rand_column_family, rand_key,
                        rand_key + FLAGS_range_deletion_width,
                        true /* pending */);

    std::string keystr = Key(rand_key);
    Slice key = keystr;
    auto cfh = column_families_[rand_column_family];
    std::string end_keystr = Key(rand_key + FLAGS_range_deletion_width);
    Slice end_key = end_keystr;
    Status s = db_->DeleteRange(write_opts, cfh, key, end_key);
    if (!s.ok()) {
      fprintf(stderr, "delete range error: %s\n", s.ToString().c_str());
      std::terminate();
    }
    int covered = shared->DeleteRange(rand_column_family, rand_key,
                                      rand_key + FLAGS_range_deletion_width,
                                      false /* pending */);
    thread->stats.AddRangeDeletions(1);
    thread->stats.AddCoveredByRangeDeletions(covered);
    return s;
  }

#ifdef ROCKSDB_LITE
  virtual void TestIngestExternalFile(
      ThreadState* /* thread */,
      const std::vector<int>& /* rand_column_families */,
      const std::vector<int64_t>& /* rand_keys */,
      std::unique_ptr<MutexLock>& /* lock */) {
    assert(false);
    fprintf(stderr,
            "RocksDB lite does not support "
            "TestIngestExternalFile\n");
    std::terminate();
  }
#else
  virtual void TestIngestExternalFile(
      ThreadState* thread, const std::vector<int>& rand_column_families,
      const std::vector<int64_t>& rand_keys, std::unique_ptr<MutexLock>& lock) {
    const std::string sst_filename =
        FLAGS_db + "/." + ToString(thread->tid) + ".sst";
    Status s;
    if (FLAGS_env->FileExists(sst_filename).ok()) {
      // Maybe we terminated abnormally before, so cleanup to give this file
      // ingestion a clean slate
      s = FLAGS_env->DeleteFile(sst_filename);
    }

    SstFileWriter sst_file_writer(EnvOptions(options_), options_);
    if (s.ok()) {
      s = sst_file_writer.Open(sst_filename);
    }
    int64_t key_base = rand_keys[0];
    int column_family = rand_column_families[0];
    std::vector<std::unique_ptr<MutexLock>> range_locks;
    std::vector<uint32_t> values;
    SharedState* shared = thread->shared;

    // Grab locks, set pending state on expected values, and add keys
    for (int64_t key = key_base;
         s.ok() && key < std::min(key_base + FLAGS_ingest_external_file_width,
                                  shared->GetMaxKey());
         ++key) {
      if (key == key_base) {
        range_locks.emplace_back(std::move(lock));
      } else if ((key & ((1 << FLAGS_log2_keys_per_lock) - 1)) == 0) {
        range_locks.emplace_back(
            new MutexLock(shared->GetMutexForKey(column_family, key)));
      }

      uint32_t value_base = thread->rand.Next() % shared->UNKNOWN_SENTINEL;
      values.push_back(value_base);
      shared->Put(column_family, key, value_base, true /* pending */);

      char value[100];
      size_t value_len = GenerateValue(value_base, value, sizeof(value));
      auto key_str = Key(key);
      s = sst_file_writer.Put(Slice(key_str), Slice(value, value_len));
    }

    if (s.ok()) {
      s = sst_file_writer.Finish();
    }
    if (s.ok()) {
      s = db_->IngestExternalFile(column_families_[column_family],
                                  {sst_filename}, IngestExternalFileOptions());
    }
    if (!s.ok()) {
      fprintf(stderr, "file ingestion error: %s\n", s.ToString().c_str());
      std::terminate();
    }
    int64_t key = key_base;
    for (int32_t value : values) {
      shared->Put(column_family, key, value, false /* pending */);
      ++key;
    }
  }
#endif  // ROCKSDB_LITE

  bool VerifyValue(int cf, int64_t key, const ReadOptions& /*opts*/,
                   SharedState* shared, const std::string& value_from_db,
                   Status s, bool strict = false) const {
    if (shared->HasVerificationFailedYet()) {
      return false;
    }
    // compare value_from_db with the value in the shared state
    char value[kValueMaxLen];
    uint32_t value_base = shared->Get(cf, key);
    if (value_base == SharedState::UNKNOWN_SENTINEL) {
      return true;
    }
    if (value_base == SharedState::DELETION_SENTINEL && !strict) {
      return true;
    }

    if (s.ok()) {
      if (value_base == SharedState::DELETION_SENTINEL) {
        VerificationAbort(shared, "Unexpected value found", cf, key);
        return false;
      }
      size_t sz = GenerateValue(value_base, value, sizeof(value));
      if (value_from_db.length() != sz) {
        VerificationAbort(shared, "Length of value read is not equal", cf, key);
        return false;
      }
      if (memcmp(value_from_db.data(), value, sz) != 0) {
        VerificationAbort(shared, "Contents of value read don't match", cf,
                          key);
        return false;
      }
    } else {
      if (value_base != SharedState::DELETION_SENTINEL) {
        VerificationAbort(shared, "Value not found: " + s.ToString(), cf, key);
        return false;
      }
    }
    return true;
  }
};

StressTest* CreateNonBatchedOpsStressTest() {
  return new NonBatchedOpsStressTest();
}

}  // namespace rocksdb
#endif  // GFLAGS
