#include <stdio.h>
#include <iostream>
#include <vector>
#include <queue>
#include <fstream>
#include <pthread.h>
#include <sys/time.h>
#include <string>
#include <semaphore.h>
#include <unistd.h>
#include <functional>
#include <algorithm>

using namespace std;

queue<std::function<void()>> tasks;
sem_t queue_sem;    // 保護 tasks queue
sem_t task_sem;     // 記錄任務數量
sem_t done_sem;     // 記錄完成任務數
bool stop = false;  // 結束信號
int active_workers = 0;

void* worker(void*) {
    while (true) {
        // 等待有任務
        sem_wait(&task_sem);

        sem_wait(&queue_sem);
        // 若 stop 且沒有任務，則退出
        if (stop && tasks.empty()) {
            sem_post(&queue_sem);
            break;
        }

        if (tasks.empty()) {
            sem_post(&queue_sem);
            continue;
        }

        auto task = std::move(tasks.front());
        tasks.pop();
        sem_post(&queue_sem);

        // 執行任務
        try {
            task();
        } catch (...) {
            // 保護，避免例外導致 thread 終止
        }

        sem_post(&done_sem);
    }
    return nullptr;
}

// 單段 bubble sort
void bubbleSortRange(vector<int>& arr, int left, int right) {
    int n = right - left;
    if (n <= 1) return;
    for (int i = 0; i < n - 1; i++) {
        bool swapped = false;
        for (int j = left; j < right - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                swap(arr[j], arr[j + 1]);
                swapped = true;
            }
        }
        if (!swapped) break;
    }
}

int main() {
    struct timeval start, end;
    vector<int> numbers;

    ifstream fin("input.txt");
    if (!fin) {
        cerr << "cannot open input.txt\n";
        return 1;
    }

    long long count;
    fin >> count;
    if (!fin) {
        cerr << "failed to read count\n";
        return 1;
    }
    numbers.reserve(count);
    for (long long i = 0; i < count; ++i) {
        int v;
        if (!(fin >> v)) break;
        numbers.push_back(v);
    }
    fin.close();

    vector<int> test_arr;

    for (int n = 1; n <= 8; n++) {
        sem_init(&queue_sem, 0, 1);
        sem_init(&task_sem, 0, 0);
        sem_init(&done_sem, 0, 0);
        stop = false;

        test_arr = numbers;
        gettimeofday(&start, NULL);

        int segs = 8;
        vector<pair<int, int>> ranges;
        int base = count / segs, rem = count % segs, idx = 0;
        for (int i = 0; i < segs; ++i) {
            int sz = base + (i < rem ? 1 : 0);
            if (sz > 0)
                ranges.push_back({idx, idx + sz});
            idx += sz;
        }

        vector<pthread_t> threads(n);
        for (int i = 0; i < n; ++i) {
            pthread_create(&threads[i], nullptr, worker, nullptr);
        }

        // distribute tasks
        for (int i = 0; i < segs; ++i) {
            int l = ranges[i].first;
            int r = ranges[i].second;
            vector<int>* arr_ptr = &test_arr;  // 用指標捕捉，避免 reference race
            sem_wait(&queue_sem);
            tasks.push([l, r, arr_ptr]() {
                bubbleSortRange(*arr_ptr, l, r);
            });
            sem_post(&queue_sem);
            sem_post(&task_sem);
        }

        // wait all done
        for (int i = 0; i < segs; ++i)
            sem_wait(&done_sem);

        // 合併所有排序段
        if (ranges.size() > 1) {
            for (size_t i = 1; i < ranges.size(); ++i) {
                int left = ranges[0].first;
                int mid = ranges[i].first;
                int right = ranges[i].second;
                inplace_merge(test_arr.begin() + left,
                              test_arr.begin() + mid,
                              test_arr.begin() + right);
            }
        }

        // 通知 thread 停止
        sem_wait(&queue_sem);
        stop = true;
        sem_post(&queue_sem);

        // 發出額外信號讓 worker 醒來退出
        for (int i = 0; i < n; ++i)
            sem_post(&task_sem);

        for (int i = 0; i < n; ++i)
            pthread_join(threads[i], nullptr);

        sem_destroy(&queue_sem);
        sem_destroy(&task_sem);
        sem_destroy(&done_sem);

        gettimeofday(&end, NULL);
        long elapsed =
            (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
        double elapsed_ms = elapsed / 1000.0;
        printf("worker thread #%d, elapsed %.3f ms\n", n, elapsed_ms);

        // 輸出結果
        ofstream fout("output_" + to_string(n) + ".txt");
        for (auto v : test_arr)
            fout << v << " ";
        fout << "\n";
        fout.close();
    }

    return 0;
}
