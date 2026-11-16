// TEMPORARY, do not merge. Stress repro for HY001 "Memory allocation error" from MySQL Connector/ODBC 9.5 on Unix.
//
// Each cycle allocates an ENV, connects from several threads at the same moment, then frees everything. Freeing
// the last ENV makes MySQL Connector/ODBC call mysql_library_end(), so the next cycle's connects re-initialize
// libmysqlclient lazily inside mysql_init(), which races. The server does not need to exist: the race is in
// mysql_init(), before any network I/O, so a refused connection is the expected outcome.
//
// Loads the ODBC library directly, either the MySQL driver itself or the wrapper with BASE_DRIVER set.
//
// Usage: hy001_repro <odbc library> <connection string> <cycles> <threads> <pin 0|1> <expect hy001|none>
//   pin 1    keeps one extra ENV of the library open for the whole run
//   expect   hy001: exit 0 only if HY001 was seen (the bug reproduced); none: exit 0 only if it was not

#include <dlfcn.h>
#include <sql.h>
#include <sqlext.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using AllocHandleFn = SQLRETURN (*)(SQLSMALLINT, SQLHANDLE, SQLHANDLE*);
using SetEnvAttrFn = SQLRETURN (*)(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER);
using DriverConnectFn = SQLRETURN (*)(SQLHDBC, SQLHWND, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*,
                                      SQLUSMALLINT);
using GetDiagRecFn = SQLRETURN (*)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT, SQLCHAR*, SQLINTEGER*, SQLCHAR*, SQLSMALLINT,
                                   SQLSMALLINT*);
using DisconnectFn = SQLRETURN (*)(SQLHDBC);
using FreeHandleFn = SQLRETURN (*)(SQLSMALLINT, SQLHANDLE);

template <typename Fn>
Fn Load(void* lib, const char* name) {
    void* sym = dlsym(lib, name);
    if (!sym) {
        std::fprintf(stderr, "Missing symbol %s: %s\n", name, dlerror());
        std::exit(2);
    }
    return reinterpret_cast<Fn>(sym);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        std::fprintf(stderr, "Usage: %s <odbc library> <connection string> <cycles> <threads> <pin 0|1> "
                             "<expect hy001|none>\n", argv[0]);
        return 2;
    }
    const std::string library = argv[1];
    const std::string conn_str = argv[2];
    const int cycles = std::atoi(argv[3]);
    const int threads = std::atoi(argv[4]);
    const bool pin = std::string(argv[5]) == "1";
    const std::string expect = argv[6];

    void* lib = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen %s failed: %s\n", library.c_str(), dlerror());
        return 2;
    }
    const auto alloc_handle = Load<AllocHandleFn>(lib, "SQLAllocHandle");
    const auto set_env_attr = Load<SetEnvAttrFn>(lib, "SQLSetEnvAttr");
    const auto driver_connect = Load<DriverConnectFn>(lib, "SQLDriverConnect");
    const auto get_diag_rec = Load<GetDiagRecFn>(lib, "SQLGetDiagRec");
    const auto disconnect = Load<DisconnectFn>(lib, "SQLDisconnect");
    const auto free_handle = Load<FreeHandleFn>(lib, "SQLFreeHandle");

    std::printf("library=%s cycles=%d threads=%d pin=%d expect=%s\n", library.c_str(), cycles, threads, pin ? 1 : 0,
                expect.c_str());

    if (pin) {
        SQLHENV pinned_env = SQL_NULL_HENV;
        if (!SQL_SUCCEEDED(alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &pinned_env))) {
            std::fprintf(stderr, "Unable to allocate the pinned ENV\n");
            return 2;
        }
        // Never freed
    }

    std::atomic<long> hy001{0};
    std::atomic<long> connects{0};
    std::mutex outcomes_mutex;
    std::map<std::string, long> outcomes;
    long cycles_with_hy001 = 0;

    const auto start = std::chrono::steady_clock::now();
    for (int cycle = 0; cycle < cycles; cycle++) {
        SQLHENV env = SQL_NULL_HENV;
        if (!SQL_SUCCEEDED(alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
            std::fprintf(stderr, "Cycle %d: unable to allocate ENV\n", cycle);
            return 2;
        }
        set_env_attr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);

        std::vector<SQLHDBC> dbcs(threads, SQL_NULL_HDBC);
        for (SQLHDBC& dbc : dbcs) {
            if (!SQL_SUCCEEDED(alloc_handle(SQL_HANDLE_DBC, env, &dbc))) {
                std::fprintf(stderr, "Cycle %d: unable to allocate DBC\n", cycle);
                return 2;
            }
        }

        const long hy001_before = hy001;
        std::barrier sync(threads);
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (int i = 0; i < threads; i++) {
            workers.emplace_back([&, i] {
                sync.arrive_and_wait();
                const SQLRETURN rc = driver_connect(dbcs[i], nullptr,
                    reinterpret_cast<SQLCHAR*>(const_cast<char*>(conn_str.c_str())), SQL_NTS, nullptr, 0, nullptr,
                    SQL_DRIVER_NOPROMPT);
                ++connects;
                std::string outcome;
                if (SQL_SUCCEEDED(rc)) {
                    outcome = "connected";
                    disconnect(dbcs[i]);
                } else {
                    SQLCHAR state[6] = {0};
                    SQLCHAR message[512] = {0};
                    SQLINTEGER native_error = 0;
                    SQLSMALLINT length = 0;
                    get_diag_rec(SQL_HANDLE_DBC, dbcs[i], 1, state, &native_error, message, sizeof(message), &length);
                    outcome = std::string(reinterpret_cast<char*>(state)) + ": " + reinterpret_cast<char*>(message);
                    if (std::string(reinterpret_cast<char*>(state)) == "HY001") {
                        ++hy001;
                    }
                }
                const std::lock_guard<std::mutex> lock(outcomes_mutex);
                ++outcomes[outcome];
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        if (hy001 > hy001_before) {
            cycles_with_hy001++;
        }

        for (SQLHDBC dbc : dbcs) {
            free_handle(SQL_HANDLE_DBC, dbc);
        }
        free_handle(SQL_HANDLE_ENV, env);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::printf("Outcomes over %ld connects in %.1fs:\n", connects.load(), seconds);
    for (const auto& [outcome, count] : outcomes) {
        std::printf("  %8ld  %s\n", count, outcome.c_str());
    }
    std::printf("RESULT: HY001=%ld in %ld of %d cycles\n", hy001.load(), cycles_with_hy001, cycles);

    if (expect == "hy001") {
        if (hy001 == 0) {
            std::printf("FAIL: expected the bug to reproduce, but no HY001 was seen\n");
            return 1;
        }
        std::printf("OK: reproduced\n");
        return 0;
    }
    if (hy001 != 0) {
        std::printf("FAIL: expected no HY001\n");
        return 1;
    }
    std::printf("OK: no HY001\n");
    return 0;
}
