#include <iostream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <list>
#include <string>
#include <sstream>

// TODO: попытаться ускорить это + проверить, что модель кешей +- правдивая

// Структура для представления строки лога
struct LogEntry {
    std::string access_type;
    uint64_t address;
    uint64_t thread_id;
    uint64_t return_address;
};

// Структура для кэш-линии
struct CacheLine {
    uint64_t tag;
    bool valid;
    uint64_t last_access_time;
    
    CacheLine() : tag(0), valid(false), last_access_time(0) {}
};

// Класс для представления одного уровня кэша
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
    Cache(size_t size_bytes, size_t line_size_bytes, size_t associativity, bool shared) 
        : size(size_bytes), line_size(line_size_bytes), associativity(associativity), 
          is_shared(shared), access_counter(0), hits(0), misses(0) {
        
        num_sets = size / (line_size * associativity);
        sets.resize(num_sets, std::vector<CacheLine>(associativity));
    }

    bool access(uint64_t address, uint64_t thread_id) {
        uint64_t set_index = (address / line_size) % num_sets;
        uint64_t tag = address / (line_size * num_sets);
        
        // Поиск в наборе
        auto& set = sets[set_index];
        for (auto& line : set) {
            if (line.valid && line.tag == tag) {
                line.last_access_time = ++access_counter;
                hits++;
                return true;  // cache hit
            }
        }

        // Cache miss - ищем место для новой линии
        misses++;
        
        // Ищем пустую линию или линию для замены
        CacheLine* replacement_line = nullptr;
        for (auto& line : set) {
            if (!line.valid) {
                replacement_line = &line;
                break;
            }
        }

        // Если нет пустых линий, используем LRU
        if (!replacement_line) {
            uint64_t oldest_time = UINT64_MAX;
            for (auto& line : set) {
                if (line.last_access_time < oldest_time) {
                    oldest_time = line.last_access_time;
                    replacement_line = &line;
                }
            }
        }

        // Обновляем линию
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

// Класс для управления иерархией кэшей
class CacheHierarchy {
private:
    std::vector<Cache> l1_caches;  // по одному на поток
    Cache l2_cache;
    Cache l3_cache;

public:
    CacheHierarchy(
        size_t num_cores,
        size_t l1_size, size_t l1_line_size, size_t l1_associativity,
        size_t l2_size, size_t l2_line_size, size_t l2_associativity,
        size_t l3_size, size_t l3_line_size, size_t l3_associativity
    ) : l2_cache(l2_size, l2_line_size, l2_associativity, true),
        l3_cache(l3_size, l3_line_size, l3_associativity, true) {
        
        // Создаем L1 кэш для каждого ядра
        for (size_t i = 0; i < num_cores; i++) {
            l1_caches.emplace_back(l1_size, l1_line_size, l1_associativity, false);
        }
    }

    void access(uint64_t address, uint64_t thread_id) {
        // TODO: не совсем верный подсчет, надо исправить
        // Пробуем L1 // Берем L1-data кеш, L1-instruction не интересует
        bool l1_hit = l1_caches[thread_id % l1_caches.size()].access(address, thread_id); // можно сделать предположние, что в принципе каждый поток на отдельном ядре
        if (l1_hit) return;

        // При промахе L1 пробуем L2
        bool l2_hit = l2_cache.access(address, thread_id);
        if (l2_hit) {
            // При попадании в L2 подгружаем также в L1
            l1_caches[thread_id % l1_caches.size()].access(address, thread_id);
            return;
        }

        // При промахе L2 пробуем L3
        bool l3_hit = l3_cache.access(address, thread_id);
        
        // При промахе L3 данные подгружаются из памяти во все уровни кэша
        l2_cache.access(address, thread_id);
        l1_caches[thread_id % l1_caches.size()].access(address, thread_id);
    }

    void print_statistics() {
        size_t l1_hits = 0, l1_misses = 0;
        size_t l2_hits = 0, l2_misses = 0;
        size_t l3_hits = 0, l3_misses = 0;

        for (const auto& l1 : l1_caches) {
            size_t hits, misses;
            l1.get_statistics(hits, misses);
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

// Функция для парсинга строки лога
LogEntry parse_log_line(const std::string& line) {
    LogEntry entry;
    std::stringstream ss(line);
    ss >> entry.access_type >> entry.address >> entry.thread_id >> entry.return_address;
    return entry;
}

int main() {
    // TODO: сделать L1 кеш полностью ассоциативным
    // Создаем иерархию кэшей с типичными параметрами
    CacheHierarchy cache_hierarchy(
        8,                          // количество ядер
        32 * 1024,                 // L1 size (32KB)
        64,                        // L1 line size
        8,                         // L1 associativity
        256 * 1024,               // L2 size (256KB)
        64,                        // L2 line size
        8,                         // L2 associativity
        8 * 1024 * 1024,          // L3 size (8MB)
        64,                        // L3 line size
        16                         // L3 associativity
    );

    std::ifstream input_file("example3.log"); // в файле 61 543 901 строк
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
