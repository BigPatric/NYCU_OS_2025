#include<iostream>
#include<vector>
#include<stdio.h>
#include<fstream>
#include<string>
#include<sstream>
#include<sys/time.h>
#include<list>
#include<unordered_map>
#include<unordered_set>
using namespace std;

struct TraceEntry{
    char op;
    unsigned long address; // Represents the page number
};

// Array of frame sizes (in pages) to simulate
int frames[5] = {4096, 8192, 16384, 32768, 65536};


void simLRU(const vector<TraceEntry>& trace){
    struct timeval start_time, end_time;
    long long ref_count = trace.size();
    gettimeofday(&start_time, NULL);
    
    printf("LRU policy:\n");
    printf("Frame\tHit\t\tMiss\t\tPage fault ratio\tWrite back count\n");
    
    for(int f=0;f<5;f++){
        int frame_n = frames[f];
        unsigned long long hit = 0, miss = 0, write_back = 0;

        // LRU list: LRU at the front, MRU (Most Recently Used) at the back
        list<unsigned long> lru_list; 
        // Map: page -> (list iterator, dirty_bit)
        unordered_map<unsigned long, pair<list<unsigned long>::iterator, bool>> mem; 

        for(const auto& entry : trace) {
            unsigned long page = entry.address;
            bool is_write = (entry.op == 'W');

            auto it = mem.find(page);
            if(it != mem.end()) {
                // Hit: Move to MRU (back of the list)
                hit++;
                if(is_write) it->second.second = true; // Set dirty bit if it's a write
                lru_list.erase(it->second.first);
                lru_list.push_back(page);
                it->second.first = prev(lru_list.end()); // Update iterator
            } else {
                // Miss
                miss++;
                if((int)lru_list.size() >= frame_n) {
                    // Cache is full, evict the LRU page (front)
                    unsigned long victim = lru_list.front();
                    if(mem.at(victim).second) write_back++; // Check if dirty before removal
                    mem.erase(victim);
                    lru_list.pop_front();
                }
                // Insert new page as MRU
                lru_list.push_back(page);
                mem[page] = {prev(lru_list.end()), is_write};
            }
        }
        double page_fault_ratio = (ref_count == 0) ? 0.0 : (double)miss / ref_count;
        printf("%d\t%lld\t%lld\t\t%.10f\t\t%lld\n", frame_n, hit, miss, page_fault_ratio, write_back);
    }
    gettimeofday(&end_time, NULL);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
    printf("Total elapsed time %.6f sec\n\n", elapsed);
}


void simCFLRU(const vector<TraceEntry>& trace){
    struct timeval start_time, end_time;
    long long ref_count = trace.size();
    gettimeofday(&start_time, NULL);

    printf("CFLRU policy:\n");
    printf("Frame\tHit\t\tMiss\t\tPage fault ratio\tWrite back count\n");

    for(int f=0;f<5;f++){
        int frame_n = frames[f];
        int cf_window = frame_n / 4; // Clean-First window size

        unsigned long long hit = 0, miss = 0, write_back = 0;

        // LRU list: front = LRU, back = MRU
        list<unsigned long> lru_list;
        // page -> (iterator, dirty)
        unordered_map<unsigned long, pair<list<unsigned long>::iterator, bool>> mem;

        for(const auto& entry : trace) {
            unsigned long page = entry.address; // 不要再 shift
            bool is_write = (entry.op == 'W');

            auto it = mem.find(page);
            if(it != mem.end()) {
                // Hit: Move to MRU
                hit++;
                if(is_write) it->second.second = true;
                lru_list.erase(it->second.first);
                lru_list.push_back(page);
                mem[page] = {prev(lru_list.end()), it->second.second};
            } else {
                // Miss
                miss++;
                if((int)lru_list.size() >= frame_n) {
                    // 先在 LRU 前 cf_window 找乾淨頁
                    auto evict_it = lru_list.begin();
                    auto evict_map_it = mem.find(*evict_it);
                    bool found_clean = false;
                    for(int i=0; i<cf_window && evict_it!=lru_list.end(); ++i, ++evict_it) {
                        evict_map_it = mem.find(*evict_it);
                        if(evict_map_it != mem.end() && !evict_map_it->second.second) {
                            found_clean = true;
                            break;
                        }
                    }
                    // 若沒找到乾淨頁，淘汰 LRU
                    if(!found_clean) evict_it = lru_list.begin();
                    unsigned long victim = *evict_it;
                    if(mem[victim].second) write_back++;
                    lru_list.erase(evict_it);
                    mem.erase(victim);
                }
                // Insert new page as MRU
                lru_list.push_back(page);
                mem[page] = {prev(lru_list.end()), is_write};
            }
        }
        double page_fault_ratio = (ref_count == 0) ? 0.0 : (double)miss / ref_count;
        printf("%d\t%lld\t%lld\t\t%.10f\t\t%lld\n", frame_n, hit, miss, page_fault_ratio, write_back);
    }
    gettimeofday(&end_time, NULL);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
    printf("Total elapsed time %.6f sec\n\n", elapsed);
}


int main(int argc, char* argv[]){
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <input_file>" << endl;
        return 1;
    }
    vector<TraceEntry> trace;
    ifstream infile(argv[1]);
    string line;
    
    // Read and parse the trace file
    while (getline(infile, line)) {
        istringstream iss(line);
        char op;
        string offset_str;
        if (iss >> op >> offset_str) {
            // Convert hexadecimal byte offset string to unsigned long
            unsigned long byte_offset = stoull(offset_str, nullptr, 16);
            // Calculate the page number (assuming 4KB page size)
            unsigned long page_num = byte_offset / 4096; 
            trace.push_back({op, page_num});
        }
    }
    infile.close();
    
    // Run simulations
    if (!trace.empty()) {
        simLRU(trace);
        simCFLRU(trace);
    } else {
        cerr << "Error: Trace file is empty or could not be read." << endl;
        return 1;
    }
    
    return 0;
}