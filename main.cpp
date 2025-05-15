#include <cmath>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct LogEntry {
    std::string access_type;
    uint64_t address;
    uint64_t thread_id;
    uint64_t return_address;
};


struct CacheLine {
    uint64_t tag;
    bool valid;
    uint64_t last_access_time;
    
    CacheLine() : tag(0), valid(false), last_access_time(0) {}
};


class Cache {
private:
    size_t size;           // размер кэша в байтах
    size_t line_size;      // размер кэш-линии в байтах
    size_t associativity;  // ассоциативность
    bool is_shared;        // общий или приватный
    size_t num_sets;       // количество сетов
    
    std::vector<std::vector<CacheLine> > sets;
    uint64_t access_counter;
    
    // Статистика
    size_t hits;
    size_t misses;

public:
    Cache() : size(0), line_size(0), associativity(0), 
            is_shared(false), num_sets(0), access_counter(0), 
            hits(0), misses(0) {
    }

    Cache(size_t size_bytes, size_t line_size_bytes, size_t associativity, bool shared) 
        : size(size_bytes), line_size(line_size_bytes), associativity(associativity), 
          is_shared(shared), access_counter(0), hits(0), misses(0) {
        
        num_sets = size / (line_size * associativity);
        sets.resize(num_sets, std::vector<CacheLine>(associativity));
    }


    bool access(uint64_t address, bool count_cache = true) {
        const uint64_t offset_bits = log2(line_size);
        const uint64_t index_bits = log2(num_sets);
        
        const uint64_t set_mask = (1ULL << index_bits) - 1;
        
        uint64_t set_index = (address >> offset_bits) & set_mask;
        uint64_t tag = address >> (offset_bits + index_bits);
        
        auto& set = sets[set_index];
        for (auto& line : set) {
            if (line.valid && line.tag == tag) {
                line.last_access_time = ++access_counter;
                if (count_cache) {
                    hits++;
                }
                return true;  // cache hit
            }
        }

        // Cache miss
        if (count_cache) {
            misses++;
        }
        
        CacheLine* replacement_line = nullptr;
        for (auto& line : set) {
            if (!line.valid) {
                replacement_line = &line;
                break;
            }
        }

        // LRU
        if (!replacement_line) {
            uint64_t oldest_time = UINT64_MAX;
            for (auto& line : set) {
                if (line.last_access_time < oldest_time) {
                    oldest_time = line.last_access_time;
                    replacement_line = &line;
                }
            }
        }

        replacement_line->valid = true;
        replacement_line->tag = tag;
        replacement_line->last_access_time = ++access_counter;
        
        return false;  // cache miss
    }

    void get_statistics(size_t& out_hits, size_t& out_misses) const {
        out_hits = hits;
        out_misses = misses;
    }
};


class CacheHierarchy {
private:
    std::map<uint64_t, Cache> l1_caches;  // по одному на поток
    Cache l2_cache;
    Cache l3_cache;

    size_t l1_size;
    size_t l1_line_size;
    size_t l1_associativity;

public:
    CacheHierarchy(
        size_t num_cores,
        size_t l1_size, size_t l1_line_size, size_t l1_associativity,
        size_t l2_size, size_t l2_line_size, size_t l2_associativity,
        size_t l3_size, size_t l3_line_size, size_t l3_associativity
    ) : l2_cache(l2_size, l2_line_size, l2_associativity, true),
        l3_cache(l3_size, l3_line_size, l3_associativity, true),
        l1_size(l1_size), l1_line_size(l1_line_size), l1_associativity(l1_associativity) {
    }

    void access(uint64_t address, uint64_t thread_id) {
        // Пробуем L1 // Берем L1-data кеш, L1-instruction не интересует
        // Предполагаем, что каждый поток на отдельном ядре
        if (l1_caches.find(thread_id) == l1_caches.end()) {
            l1_caches[thread_id] = std::move(Cache(l1_size, l1_line_size, l1_associativity, false));
        }

        bool l1_hit = l1_caches[thread_id].access(address);
        if (l1_hit) return;

        // При промахе L1 пробуем L2
        bool l2_hit = l2_cache.access(address);
        if (l2_hit) {
            // При попадании в L2 подгружаем также в L1
            l1_caches[thread_id].access(address, false);
            return;
        }

        // При промахе L2 пробуем L3
        bool l3_hit = l3_cache.access(address, thread_id);
        
        // При промахе L3 данные подгружаются из памяти во все уровни кэша
        l2_cache.access(address, false);
        l1_caches[thread_id].access(address, false);
    }

    void print_statistics() {
        size_t l1_hits = 0, l1_misses = 0;
        size_t l2_hits = 0, l2_misses = 0;
        size_t l3_hits = 0, l3_misses = 0;

        for (const auto& l1 : l1_caches) {
            size_t hits, misses;
            l1.second.get_statistics(hits, misses);
            l1_hits += hits;
            l1_misses += misses;
        }

        l2_cache.get_statistics(l2_hits, l2_misses);
        l3_cache.get_statistics(l3_hits, l3_misses);

        std::cout << "Cache Statistics:\n"
                  << "L1: " << l1_hits << " hits, " << l1_misses << " misses\n"
                  << "L2: " << l2_hits << " hits, " << l2_misses << " misses\n"
                  << "L3: " << l3_hits << " hits, " << l3_misses << " misses\n";
    }
};


LogEntry parse_log_line(const std::string& line) {
    LogEntry entry;
    std::stringstream ss(line);
    ss >> entry.access_type >> entry.address >> entry.thread_id >> entry.return_address;
    return entry;
}

int main() {
    // Можно промедилировать полностью ассоциативный кеш, подобрав нужную ассоциативность
    CacheHierarchy cache_hierarchy(
        78,                          // количество ядер
        5 * 1024 * 1024,                 // L1 size (5 MiB)
        64,                        // L1 line size
        8,                         // L1 associativity
        39 * 1024 * 1024,               // L2 size (39 MiB)
        64,                        // L2 line size
        8,                         // L2 associativity
        6 * 1024 * 1024,          // L3 size (64 MiB)
        64,                        // L3 line size
        16                         // L3 associativity
    );

    std::ifstream input_file("memory_trace.log");
    std::string line;

    uint64_t i = 0; 
    while (std::getline(input_file, line)) {
        if (++i % 10000 == 0) {
            std::cout << "Proccess " << i << " line" << std::endl;
        }
        LogEntry entry = parse_log_line(line);
        cache_hierarchy.access(entry.address, entry.thread_id);
    }

    cache_hierarchy.print_statistics();
    return 0;
}
