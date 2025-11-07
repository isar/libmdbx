#include "mdbx.h++"

#include <chrono>
#include <deque>
#include <iostream>
#include <vector>
#if defined(__cpp_lib_latch) && __cpp_lib_latch >= 201907L
#include <latch>
#include <thread>
#endif

#if defined(ENABLE_MEMCHECK) || defined(MDBX_CI)
#if MDBX_DEBUG || !defined(NDEBUG)
#define RELIEF_FACTOR 16
#else
#define RELIEF_FACTOR 8
#endif
#elif MDBX_DEBUG || !defined(NDEBUG) || defined(__APPLE__) || defined(_WIN32)
#define RELIEF_FACTOR 4
#elif UINTPTR_MAX > 0xffffFFFFul || ULONG_MAX > 0xffffFFFFul
#define RELIEF_FACTOR 2
#else
#define RELIEF_FACTOR 1
#endif

// static const auto NN = 1000u / RELIEF_FACTOR;

#if defined(__cpp_lib_latch) && __cpp_lib_latch >= 201907L
static const auto N = std::min(17u, std::thread::hardware_concurrency());
#else
static const auto N = 3u;
#endif

static void logger_nofmt(MDBX_log_level_t loglevel, const char *function, int line, const char *msg,
                         unsigned length) noexcept {
  (void)length;
  (void)loglevel;
  fflush(nullptr);
  std::cout << function << ":" << line << " " << msg;
  std::cout.flush();
}

static char log_buffer[1024];

//--------------------------------------------------------------------------------------------

typedef MDBX_cache_result_t (*get_cached_t)(const MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key, MDBX_val *data,
                                            MDBX_cache_entry_t *entry);

static bool check_state(const MDBX_cache_result_t &r, const MDBX_error_t wanna_errcode,
                        const MDBX_cache_status_t wanna_status, unsigned line) {
  if (r.errcode == wanna_errcode && r.status == wanna_status)
    return true;
  std::cerr << "unecpected (at " << line
            << "): "
               "err "
            << r.errcode << " (wanna " << wanna_errcode
            << "), "
               "status "
            << r.status << " (wanna " << wanna_status << ")" << std::endl;
  return false;
}

static bool check_state_and_value(const MDBX_cache_result_t &r, const mdbx::slice &value,
                                  const MDBX_error_t wanna_errcode, const MDBX_cache_status_t wanna_status,
                                  const mdbx::slice &wanna_value, unsigned line) {

  bool ok = check_state(r, wanna_errcode, wanna_status, line);
  if (value != wanna_value) {
    std::cerr << "mismatch value (at " << line << "): " << value << " (wanna " << wanna_value << ")" << std::endl;
    ok = false;
  }
  return ok;
}

bool case0_trivia(mdbx::env env) {
  get_cached_t get_cached = mdbx_cache_get_SingleThreaded;
  auto txn = env.start_write();
  auto table = txn.create_map("case0", mdbx::key_mode::usual, mdbx::value_mode::single);

  MDBX_cache_entry_t entry;
  mdbx_cache_init(&entry);
  MDBX_val data;
  MDBX_cache_result_t r;

  bool ok = true;
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state(r, MDBX_NOTFOUND, MDBX_CACHE_DIRTY, __LINE__) && ok;

  txn.commit_embark_read();
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state(r, MDBX_NOTFOUND, MDBX_CACHE_REFRESHED, __LINE__) && ok;

  // drops the table as if it were done by another process
  {
    auto params = mdbx::env::operate_parameters(42);
    params.options.no_sticky_threads = true;
    mdbx::env_managed env2(env.get_path(), params);
    auto txn2 = env2.start_write();
    txn2.drop_map("case0");
    txn2.commit();
  }
  txn.renew_reading();
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state(r, MDBX_NOTFOUND, MDBX_CACHE_CONFIRMED, __LINE__) && ok;

  txn.abort();
  txn = env.start_write();
  table = txn.create_map("case0", mdbx::key_mode::usual, mdbx::value_mode::single);
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state(r, MDBX_NOTFOUND, MDBX_CACHE_DIRTY, __LINE__) && ok;

  txn.commit_embark_read();
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state(r, MDBX_NOTFOUND, MDBX_CACHE_CONFIRMED, __LINE__) && ok;

  txn.abort();
  txn = env.start_write();
  txn.insert(table, "key", "value");
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state_and_value(r, data, MDBX_SUCCESS, MDBX_CACHE_DIRTY, "value", __LINE__) && ok;

  txn.commit_embark_read();
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state_and_value(r, data, MDBX_SUCCESS, MDBX_CACHE_REFRESHED, "value", __LINE__) && ok;

  txn.abort();
  txn = env.start_write();
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state_and_value(r, data, MDBX_SUCCESS, MDBX_CACHE_HIT, "value", __LINE__) && ok;
  txn.update(table, "key", "42");
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state_and_value(r, data, MDBX_SUCCESS, MDBX_CACHE_DIRTY, "42", __LINE__) && ok;

  txn.commit_embark_read();
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state_and_value(r, data, MDBX_SUCCESS, MDBX_CACHE_REFRESHED, "42", __LINE__) && ok;

  MDBX_cache_entry_t entry2;
  mdbx_cache_init(&entry2);
  txn.abort();
  txn = env.start_write();
  txn.insert(table, "key2", "value2");
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state_and_value(r, data, MDBX_SUCCESS, MDBX_CACHE_DIRTY, "42", __LINE__) && ok;
  r = get_cached(txn, table, mdbx::slice("key2"), &data, &entry2);
  ok = check_state_and_value(r, data, MDBX_SUCCESS, MDBX_CACHE_DIRTY, "value2", __LINE__) && ok;

  txn.commit_embark_read();
  r = get_cached(txn, table, mdbx::slice("key"), &data, &entry);
  ok = check_state_and_value(r, data, MDBX_SUCCESS, MDBX_CACHE_REFRESHED, "42", __LINE__) && ok;
  r = get_cached(txn, table, mdbx::slice("key2"), &data, &entry2);
  ok = check_state_and_value(r, data, MDBX_SUCCESS, MDBX_CACHE_REFRESHED, "value2", __LINE__) && ok;

  return ok;
}

//--------------------------------------------------------------------------------------------

int doit() {
  mdbx::path db_filename = "test-get-cached";
  mdbx::env::remove(db_filename);

  mdbx::env_managed env(db_filename, mdbx::env_managed::create_parameters(),
                        mdbx::env::operate_parameters(N + 2, 0, mdbx::env::nested_transactions));

  bool ok = case0_trivia(env);
  // ok = case1(env) && ok;
  // ok = case2(env) && ok;

  if (ok) {
    std::cout << "OK\n";
    return EXIT_SUCCESS;
  } else {
    std::cout << "FAIL!\n";
    return EXIT_FAILURE;
  }
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  mdbx_setup_debug_nofmt(MDBX_LOG_NOTICE, MDBX_DBG_ASSERT | MDBX_DBG_LEGACY_MULTIOPEN, logger_nofmt, log_buffer,
                         sizeof(log_buffer));
  try {
    return doit();
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
