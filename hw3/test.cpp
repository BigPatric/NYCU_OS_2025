#include <stdio.h>
#include <iostream>
#include <vector>
#include <queue>
#include <fstream>
#include <pthread.h>
#include <sys/time.h>
#include <string>
#include <unistd.h>
#include <functional>
#include <atomic>
#include <algorithm>

using namespace std;

// --- bubble sort on [left, right) ---
void bubbleSortRange(vector<int>& arr, int left, int right) {
    int n = right - left;
    if (n <= 1) return;
    for (int i = 0; i < n - 1; ++i) {
        bool swapped = false;
        for (int j = left; j < right - i - 1; ++j) {
            if (arr[j] > arr[j + 1]) {
                swap(arr[j], arr[j + 1]);
                swapped = true;
            }
        }
        if (!swapped) break;
    }
}

// --- ThreadPool using pthreads, pthread_mutex + pthread_cond ---
struct ThreadPool {
    pthread_t *threads = nullptr;
    int thread_count = 0;

    queue<std::function<void()>> tasks;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

    // for waiting all tasks done
    pthread_cond_t done_cond = PTHREAD_COND_INITIALIZER;
    atomic<int> tasks_total{0};
    atomic<int> tasks_done{0};

    bool stop = false;

    static void* workerEntry(void* arg) {
        ThreadPool* pool = static_cast<ThreadPool*>(arg);
        while (true) {
            std::function<void()> task;
            // lock and wait for task or stop
            pthread_mutex_lock(&pool->mutex);
            while (pool->tasks.empty() && !pool->stop) {
                pthread_cond_wait(&pool->cond, &pool->mutex);
            }
            if (pool->stop && pool->tasks.empty()) {
                pthread_mutex_unlock(&pool->mutex);
                break;
            }
            task = std::move(pool->tasks.front());
            pool->tasks.pop();
            pthread_mutex_unlock(&pool->mutex);

            // execute task outside lock
            try {
                task();
            } catch (...) {
                // swallow exceptions to avoid terminating worker
            }

            // notify completion
            pool->tasks_done.fetch_add(1);
            pthread_mutex_lock(&pool->mutex);
            pthread_cond_signal(&pool->done_cond);
            pthread_mutex_unlock(&pool->mutex);
        }
        return nullptr;
    }

    void start(int n) {
        thread_count = n > 0 ? n : 1;
        threads = new pthread_t[thread_count];
        stop = false;
        tasks_total = 0;
        tasks_done = 0;
        for (int i = 0; i < thread_count; ++i) {
            pthread_create(&threads[i], nullptr, &ThreadPool::workerEntry, this);
        }
    }

    void submit(std::function<void()> f) {
        pthread_mutex_lock(&mutex);
        tasks.push(std::move(f));
        tasks_total.fetch_add(1);
        pthread_cond_signal(&cond); // wake one worker
        pthread_mutex_unlock(&mutex);
    }

    // wait until all submitted tasks are done (non-blocking new submissions allowed)
    void wait_all() {
        pthread_mutex_lock(&mutex);
        while (tasks_done.load() < tasks_total.load()) {
            pthread_cond_wait(&done_cond, &mutex);
        }
        pthread_mutex_unlock(&mutex);
    }

    // shutdown: wait current tasks finished and stop workers
    void shutdown() {
        // ensure all tasks processed
        wait_all();

        pthread_mutex_lock(&mutex);
        stop = true;
        pthread_cond_broadcast(&cond); // wake all workers to let them exit
        pthread_mutex_unlock(&mutex);

        // join threads
        for (int i = 0; i < thread_count; ++i) {
            pthread_join(threads[i], nullptr);
        }
        delete[] threads;
        threads = nullptr;

        // reset counts (optional)
        tasks_total = 0;
        tasks_done = 0;
    }
};


int main() {
    struct timeval start, end;

    // read input
    ifstream fin("input.txt");
    if (!fin) {
        cerr << "cannot open input.txt\n";
        return 1;
    }
    long long count_ll;
    fin >> count_ll;
    if (!fin) {
        cerr << "failed to read count\n";
        return 1;
    }
    if (count_ll < 0) {
        cerr << "invalid count\n";
        return 1;
    }
    size_t count = static_cast<size_t>(count_ll);

    vector<int> numbers;
    numbers.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        int v;
        if (!(fin >> v)) break;
        numbers.push_back(v);
    }
    fin.close();

    if (numbers.size() != count) {
        cerr << "Warning: read " << numbers.size() << " numbers, expected " << count << "\n";
        count = numbers.size();
    }

    for (int n = 1; n <= 8; ++n) {
        // prepare array to sort
        vector<int> test_arr = numbers; // full copy for each run

        gettimeofday(&start, NULL);

        // create segments (8 segments like original code)
        const int segs = 8;
        vector<pair<int,int>> ranges;
        size_t base = count / segs;
        size_t rem = count % segs;
        size_t idx = 0;
        for (int i = 0; i < segs; ++i) {
            size_t sz = base + (i < (int)rem ? 1 : 0);
            if (sz > 0) {
                ranges.push_back({static_cast<int>(idx), static_cast<int>(idx + sz)}); // [l, r)
            }
            idx += sz;
        }

        // start thread pool
        ThreadPool pool;
        pool.start(n);

        // submit one bubbleSortRange task per segment
        for (size_t i = 0; i < ranges.size(); ++i) {
            int l = ranges[i].first;
            int r = ranges[i].second;
            // capture a pointer to the shared vector (valid until shutdown)
            vector<int>* arr_ptr = &test_arr;
            pool.submit([l, r, arr_ptr]() {
                bubbleSortRange(*arr_ptr, l, r);
            });
        }

        // wait all tasks done
        pool.wait_all();

        // Now merge all sorted segments into final sorted array
        // segments are contiguous and non-overlapping; do successive inplace_merge:
        if (ranges.size() > 1) {
            for (size_t i = 1; i < ranges.size(); ++i) {
                int left = ranges[0].first;                 // should be 0
                int mid  = ranges[i].first;                 // boundary
                int right = ranges[i].second;               // end of this segment
                // inplace_merge(begin, begin+mid, begin+right)
                inplace_merge(test_arr.begin() + left, test_arr.begin() + mid, test_arr.begin() + right);
                // after first iteration ranges[0].first..ranges[i].second becomes one sorted prefix
                // note: next iteration will merge that prefix with next segment (works with this loop)
            }
        }

        // shutdown workers
        pool.shutdown();

        gettimeofday(&end, NULL);
        long elapsed = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
        double elapsed_ms = elapsed / 1000.0;
        printf("worker thread count %d, elapsed %.6f ms\n", n, elapsed_ms);

        // write output
        ofstream fout("output_" + to_string(n) + ".txt");
        if (!fout) {
            cerr << "cannot open output file for writing\n";
            continue;
        }
        for (size_t i = 0; i < test_arr.size(); ++i) {
            fout << test_arr[i];
            if (i + 1 < test_arr.size()) fout << ' ';
        }
        fout << '\n';
        fout.close();
    }

    return 0;
}
