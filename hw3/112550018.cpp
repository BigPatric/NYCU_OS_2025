#include<stdio.h>
#include<iostream>
#include<vector>
#include<fstream>
#include<pthread.h>
#include<sys/time.h>
#include<string>
#include<algorithm>
using namespace std;

// merge [L ... M-1] and [M ... R-1]
void merge_range(vector<int> &a, vector<int> &tmp, size_t L, size_t M, size_t R){
    size_t i = L, j = M, k = L;
    while(i < M && j < R){
        if(a[i] <= a[j]) tmp[k++] = a[i++];
        else tmp[k++] = a[j++];
    }
    while(i < M) tmp[k++] = a[i++];
    while(j < R) tmp[k++] = a[j++];
    for(k = L; k < R; ++k) a[k] = tmp[k];
}

// ---------- pthread-based parallel merge sort ----------

struct ThreadArg {
    vector<int>* a;
    vector<int>* tmp;
    size_t L, R;
};

static int max_threads = 1;                // set from main (包括 main thread)
static int cur_threads = 1;                // currently active threads (count main)
static pthread_mutex_t th_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t th_cv = PTHREAD_COND_INITIALIZER;
static size_t spawn_threshold = 1024;     // 若區間小於此則不要再 spawn

// forward declaration
void merge_sort_rec_threaded(vector<int> &a, vector<int> &tmp, size_t L, size_t R);

void* thread_sort_func(void* arg){
    ThreadArg* ta = (ThreadArg*)arg;
    merge_sort_rec_threaded(*ta->a, *ta->tmp, ta->L, ta->R);
    delete ta;
    // decrement active thread count and signal waiters
    pthread_mutex_lock(&th_mtx);
    cur_threads--;
    pthread_cond_signal(&th_cv);
    pthread_mutex_unlock(&th_mtx);
    return nullptr;
}

// threaded-aware recursive sort (uses mutex/cond to limit concurrent threads)
void merge_sort_rec_threaded(vector<int> &a, vector<int> &tmp, size_t L, size_t R){
    if(R - L <= 1) return;
    size_t M = L + (R - L) / 2;

    bool spawned = false;
    pthread_t th;

    // decide whether to spawn a new thread for left half
    if((R - L) > spawn_threshold){
        pthread_mutex_lock(&th_mtx);
        if(cur_threads < max_threads){
            cur_threads++;
            spawned = true;
        }
        pthread_mutex_unlock(&th_mtx);
    }

    if(spawned){
        ThreadArg* ta = new ThreadArg{&a, &tmp, L, M};
        if(pthread_create(&th, nullptr, thread_sort_func, ta) != 0){
            // create failed -> rollback
            delete ta;
            pthread_mutex_lock(&th_mtx);
            cur_threads--;
            pthread_mutex_unlock(&th_mtx);
            spawned = false;
        }
    }

    if(spawned){
        // current thread handles right half concurrently
        merge_sort_rec_threaded(a, tmp, M, R);
        // wait for left child to finish before merging
        pthread_join(th, nullptr);
    } else {
        // no thread created -> sequential recursion
        merge_sort_rec_threaded(a, tmp, L, M);
        merge_sort_rec_threaded(a, tmp, M, R);
    }

    merge_range(a, tmp, L, M, R);
}

void merge_sort_with_pthreads(vector<int> &a, int workers){
    if(a.empty()) return;
    vector<int> tmp(a.size());
    // set limits: treat 'workers' as maximum concurrent threads including main
    if(workers < 1) workers = 1;
    max_threads = workers;
    // reset current active thread count to account for main thread
    pthread_mutex_lock(&th_mtx);
    cur_threads = 1;
    pthread_mutex_unlock(&th_mtx);

    merge_sort_rec_threaded(a, tmp, 0, a.size());

    // wait until all spawned threads finished (cur_threads == 1)
    pthread_mutex_lock(&th_mtx);
    while(cur_threads > 1){
        pthread_cond_wait(&th_cv, &th_mtx);
    }
    pthread_mutex_unlock(&th_mtx);
}

// ---------- end parallel merge sort ----------

int main(){
    struct timeval start, end;
    vector<int> numbers;

    ifstream fin("input.txt");
    if(!fin){
        cerr << "cannot open input.txt\n";
        return 1;
    }
    long long count;
    fin >> count;
    if(!fin){
        cerr << "failed to read count\n";
        return 1;
    }
    numbers.reserve((size_t)min<long long>(count, (long long)1000000000));
    for(long long i = 0; i < count; ++i){
        int v;
        if(!(fin >> v)) break;
        numbers.push_back(v);
    }
    fin.close();

    // adjust spawn_threshold if you want fewer/more spawns
    spawn_threshold = 1024;

    for(int n = 1; n <= 8; n++){
        vector<int> test_nums = numbers;
        gettimeofday(&start, NULL);

        // call pthread-parallel merge sort with 'n' max concurrent threads (including main)
        merge_sort_with_pthreads(test_nums, n);

        gettimeofday(&end, NULL);
        long elapsed = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
        double elapsed_ms = elapsed / 1000.0;
        
        printf("worker thread #%d:, elapsed %.6f ms\n", n, elapsed_ms);

        {
            string filename = "output_" + to_string(n) + ".txt";
            ofstream fout(filename);
            if(!fout){
                cerr << "cannot open " << filename << '\n';
            } else {
                for(size_t i = 0; i < test_nums.size(); ++i){
                    fout << test_nums[i];
                    if(i + 1 < test_nums.size()) fout << ' ';
                }
                fout << '\n';
                fout.close();
            }
        }

    }

    return 0;
}