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

// bbs
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

struct ThreadPool {
    pthread_t *threads = nullptr;
    int thread_count = 0;

    queue<std::function<void()>> tasks;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

    pthread_cond_t done_cond = PTHREAD_COND_INITIALIZER;
    atomic<int> tasks_total{0};
    atomic<int> tasks_done{0};

    bool stop = false;

    // Worker thread entry function
    static void* workerEntry(void* arg) {
        ThreadPool* pool = static_cast<ThreadPool*>(arg);
        while (true) {
            std::function<void()> task;
            pthread_mutex_lock(&pool->mutex);
            while (pool->tasks.empty() && !pool->stop) {
                pthread_cond_wait(&pool->cond, &pool->mutex);
            }
            if (pool->stop && pool->tasks.empty()) {
                pthread_mutex_unlock(&pool->mutex);
                break;
            }
            if (!pool->tasks.empty()) {
                task = std::move(pool->tasks.front());
                pool->tasks.pop();
            }
            pthread_mutex_unlock(&pool->mutex);

            if (task) {
                try {
                    task();
                } catch (...) {
                }
                pool->tasks_done.fetch_add(1);
                pthread_mutex_lock(&pool->mutex);
                pthread_cond_signal(&pool->done_cond);
                pthread_mutex_unlock(&pool->mutex);
            }
        }
        return nullptr;
    }

    // create n threads
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

    // submit a task
    void submit(std::function<void()> f) {
        pthread_mutex_lock(&mutex);
        tasks.push(std::move(f));
        tasks_total.fetch_add(1);
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mutex);
    }

    // wait all tasks done
    void wait_all() {
        pthread_mutex_lock(&mutex);
        while (tasks_done.load() < tasks_total.load()) {
            pthread_cond_wait(&done_cond, &mutex);
        }
        pthread_mutex_unlock(&mutex);
    }

    // shutdown the pool
    void shutdown() {
        wait_all();

        pthread_mutex_lock(&mutex);
        stop = true;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mutex);

        for (int i = 0; i < thread_count; ++i) {
            pthread_join(threads[i], nullptr);
        }
        delete[] threads;
        threads = nullptr;

        tasks_total = 0;
        tasks_done = 0;
    }
};

struct TaskNode {
    int left;
    int mid;
    int right;
    atomic<int> deps{0};
    vector<TaskNode*> parents;
    function<void()> action;
};

struct Waiter {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    bool done = false;
};

void schedule_sort_and_merge(vector<int>& arr, vector<pair<int,int>>& ranges, ThreadPool& pool) {
    vector<TaskNode*> leaves;
    vector<TaskNode*> all_tasks;
    Waiter waiter;

    // queue 8 segments sorting tasks
    for (auto &pr : ranges) {
        int l = pr.first;
        int r = pr.second;
        TaskNode* node = new TaskNode();
        node->left = l;
        node->mid = -1;
        node->right = r;
        node->deps.store(0);
        node->action = [node, &arr, &pool]() {
            bubbleSortRange(arr, node->left, node->right);
            for (TaskNode* p : node->parents) {
                int prev = p->deps.fetch_sub(1);
                if (prev == 1) {
                    pool.submit(p->action);
                }
            }
        };
        leaves.push_back(node);
        all_tasks.push_back(node);
    }

    // build merge tree
    vector<TaskNode*> current = leaves;
    while (current.size() > 1) {
        vector<TaskNode*> next;
        for (size_t i = 0; i + 1 < current.size(); i += 2) {
            TaskNode* left = current[i];
            TaskNode* right = current[i+1];
            TaskNode* parent = new TaskNode();
            parent->left = left->left;
            parent->mid = left->right;
            parent->right = right->right;
            parent->deps.store(2);
            parent->action = [parent, left, right, &arr, &pool, &waiter]() {
                inplace_merge(arr.begin() + parent->left,
                              arr.begin() + parent->mid,
                              arr.begin() + parent->right);
                for (TaskNode* p : parent->parents) {
                    int prev = p->deps.fetch_sub(1);
                    if (prev == 1) {
                        pool.submit(p->action);
                    }
                }
                if (parent->parents.empty()) {
                    pthread_mutex_lock(&waiter.mutex);
                    waiter.done = true;
                    pthread_cond_signal(&waiter.cond);
                    pthread_mutex_unlock(&waiter.mutex);
                }
            };
            left->parents.push_back(parent);
            right->parents.push_back(parent);

            next.push_back(parent);
            all_tasks.push_back(parent);
        }
        if (current.size() % 2 == 1) {
            next.push_back(current.back());
        }
        current.swap(next);
    }

    TaskNode* root = current.front();
    if (!root) {
        return;
    }
    if (root->parents.empty() && root->deps.load() == 0 && root->mid == -1) {
        TaskNode* leaf = root;
        auto orig = leaf->action;
        leaf->action = [orig, &waiter]() {
            orig();
            pthread_mutex_lock(&waiter.mutex);
            waiter.done = true;
            pthread_cond_signal(&waiter.cond);
            pthread_mutex_unlock(&waiter.mutex);
        };
    } else {
        if (root->mid == -1) {
            TaskNode* leaf = root;
            auto orig = leaf->action;
            leaf->action = [orig, &waiter]() {
                orig();
                pthread_mutex_lock(&waiter.mutex);
                waiter.done = true;
                pthread_cond_signal(&waiter.cond);
                pthread_mutex_unlock(&waiter.mutex);
            };
        }
    }

    for (TaskNode* leaf : leaves) {
        pool.submit(leaf->action);
    }

    pthread_mutex_lock(&waiter.mutex);
    while (!waiter.done) {
        pthread_cond_wait(&waiter.cond, &waiter.mutex);
    }
    pthread_mutex_unlock(&waiter.mutex);

    for (TaskNode* t : all_tasks) {
        delete t;
    }
}

int main() {
    struct timeval start, end;

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
        vector<int> test_arr = numbers;

        // determine 8 segments
        const int segs = 8;
        vector<pair<int,int>> ranges;
        size_t base = (count == 0 ? 0 : count / segs);
        size_t rem = (count == 0 ? 0 : count % segs);
        size_t idx = 0;
        for (int i = 0; i < segs; ++i) {
            size_t sz = base + (i < (int)rem ? 1 : 0);
            if (sz > 0) {
                ranges.push_back({static_cast<int>(idx), static_cast<int>(idx + sz)});
            }
            idx += sz;
        }

        gettimeofday(&start, NULL);


        ThreadPool pool;
        pool.start(n);

        schedule_sort_and_merge(test_arr, ranges, pool);

        pool.shutdown();

        gettimeofday(&end, NULL);
        long elapsed = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
        double elapsed_ms = elapsed / 1000.0;
        printf("worker thread #%d, elapsed %.6f ms\n", n, elapsed_ms);

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
