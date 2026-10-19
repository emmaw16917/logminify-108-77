#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <map>
#include <list>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <iterator>
#include <regex>
#include <cassert>
#include <thread>
#include <mutex>
#include <future>

namespace fs = std::filesystem;
std::mutex mtx;

// Zigzag/elastic decode
int64_t zigzag_decode(int64_t num) {
    return (num >> 1) ^ -(num & 1);
}
int64_t elastic_decode(const std::vector<unsigned char>& num_bytes) {
    int64_t ret = 0;
    int offset = 0;
    for (auto cur : num_bytes) {
        ret |= (static_cast<int64_t>(cur & 0x7F) << offset);
        if ((cur & 0x80) == 0) break;
        offset += 7;
    }
    return zigzag_decode(ret);
}
std::vector<int64_t> elastic_decode_bytes(const std::vector<unsigned char>& binary_bytes) {
    std::vector<int64_t> num_list;
    std::vector<unsigned char> num_byte;
    for (auto byt : binary_bytes) {
        num_byte.push_back(byt);
        if (byt < 128) {
            int64_t decode_num = elastic_decode(num_byte);
            num_list.push_back(decode_num);
            num_byte.clear();
        }
    }
    return num_list;
}
// 逆delta
std::list<int64_t> delta_restore(const std::list<int64_t>& delta_list) {
    std::list<int64_t> restored;
    if (delta_list.empty()) return restored;
    auto it = delta_list.begin();
    int64_t last = *it;
    restored.push_back(last);
    ++it;
    for (; it != delta_list.end(); ++it) {
        last += *it;
        restored.push_back(last);
    }
    return restored;
}

// 读取二进制文件并elastic解码
std::list<int64_t> read_bin_numbers(const std::string& filename, bool use_delta) {
    std::ifstream file(filename, std::ios::binary);
    std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto nums = elastic_decode_bytes(buffer);
    std::list<int64_t> num_list(nums.begin(), nums.end());
    if (use_delta) return delta_restore(num_list);
    return num_list;
}

// 读取变量内容
std::vector<std::string> read_mapping(const std::string& filename) {
    std::ifstream file(filename);
    std::vector<std::string> mapping;
    std::string line;
    while (std::getline(file, line)) mapping.push_back(line);
    return mapping;
}
// 读取id序列
std::vector<int> read_ids(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto ids64 = elastic_decode_bytes(buffer);
    std::vector<int> ids;
    for (auto v : ids64) ids.push_back(static_cast<int>(v));
    return ids;
}

// 解压xz
void decompress_xz(const std::string& xzfile, const std::string& outdir) {
    std::string cmd = "tar -xJf " + xzfile + " -C " + outdir;
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "Failed to decompress: " << xzfile << std::endl;
        exit(1);
    }
}

// 查找所有block
std::vector<int> find_block_ids(const std::string& outdir) {
    std::vector<int> ids;
    for (const auto& entry : fs::directory_iterator(outdir)) {
        std::string name = entry.path().filename().string();
        if (name.find("compressed") == 0 && name.find(".xz") != std::string::npos) {
            std::string num = name.substr(10, name.size() - 13);
            ids.push_back(std::stoi(num));
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// 查找所有 *.bin 文件（不包括 variablesetids.bin 和 allids.bin）
std::vector<fs::path> find_bin_files(const std::string& block_dir) {
    std::vector<fs::path> bin_files;
    for (const auto& entry : fs::directory_iterator(block_dir)) {
        auto fname = entry.path().filename().string();
        if (fname.size() > 4 && fname.substr(fname.size()-4) == ".bin" &&
            fname != "variablesetids.bin" && fname != "allids.bin") {
            bin_files.push_back(entry.path());
        }
    }
    return bin_files;
}

// 替换所有占位符
void replace_placeholders(std::vector<std::string>& logs, const std::string& placeholder, const std::vector<std::string>& replacements) {
    auto it = replacements.begin();
    for (auto& log : logs) {
        size_t pos = 0;
        while ((pos = log.find(placeholder, pos)) != std::string::npos && it != replacements.end()) {
            log.replace(pos, placeholder.size(), *it);
            ++it;
            pos += 1;
        }
    }
}

// 替换所有占位符（数字型）
void replace_placeholders_num(std::vector<std::string>& logs, const std::string& placeholder, const std::vector<int64_t>& replacements) {
    auto it = replacements.begin();
    for (auto& log : logs) {
        size_t pos = 0;
        while ((pos = log.find(placeholder, pos)) != std::string::npos && it != replacements.end()) {
            log.replace(pos, placeholder.size(), std::to_string(*it));
            ++it;
            pos += 1;
        }
    }
}

// 获取当前 logname 的占位符与格式对应关系
std::map<std::string, std::string> get_placeholder_formats(const std::string& logname) {
    if (logname == "Android") {
        return {
            {"<I>", R"((\d+)\.(\d+)\.(\d+)\.(\d+))"},
            {"<T>", R"((\d+)-(\d+) (\d+):(\d+):(\d+)(?:\.(\d+))?)"}
        };
    } else if (logname == "Apache") {
        return {
            {"<I>", R"((\d+)\.(\d+)\.(\d+)\.(\d+))"},
            {"<T>", R"((\d{2}) (\d+):(\d+):(\d+))"}
        };
    } else if (logname == "BGL") {
        return {
            {"<E>", R"((\d+)-(\d+)-(\d+)-(\d+)\.(\d+)\.(\d+))"},
            {"<T>", R"((\d+):(\d+):(\d+))"},
            {"<F>", R"((\d+)\.(\d+)\.(\d+))"}
        };
    } else if (logname == "Hadoop") {
        return {
            {"<D>", R"((\d+)\-(\d+)\-(\d+))"},
            {"<T>", R"((\d+):(\d+):(\d+),(\d+))"}
        };
    } else if (logname == "HDFS") {
        return {
            {"<I>", R"((\d+)\.(\d+)\.(\d+)\.(\d+))"},
            {"<T>", R"((\d+):(\d+):(\d+),(\d+))"}
        };
    } else if (logname == "HealthApp") {
        return {
            {"<T>", R"((\d+):(\d+):(\d+):(\d+))"}
        };
    } else if (logname == "HPC") {
        return {
            {"<I>", R"((\d+)\.(\d+)\.(\d+)\.(\d+))"},
            {"<T>", R"((\d+)-(\d+) (\d+):(\d+):(\d+)(?:\.(\d+))?)"}
        };
    } else if (logname == "Linux") {
        return {
            {"<I>", R"((\d+)\.(\d+)\.(\d+)\.(\d+))"},
            {"<T>", R"((\d+)-(\d+) (\d+):(\d+):(\d+)(?:\.(\d+))?)"}
        };
    } else if (logname == "Mac") {
        return {
            {"<D>", R"((\d+)-(\d+)-(\d+)-(\d+))"},
            {"<T>", R"((\d+):(\d+):(\d+)(?:\.(\d+))?)"}
        };
    } else if (logname == "OpenSSH") {
        return {
            {"<I>", R"((\d+)\.(\d+)\.(\d+)\.(\d+))"},
            {"<T>", R"((\d+) (\d+):(\d+):(\d+)(?:\.(\d+))?)"},
            {"<S>", R"(sshd\[(\d+)\]:)"}
        };
    } else if (logname == "OpenStack") {
        return {
            {"<D>", R"(\.(\d+)-(\d+)-(\d+)_...)"},
            {"<T>", R"((\d+)-(\d+)-(\d+).(\d+):(\d+):(\d+)\.(\d+))"}
        };
    } else if (logname == "Proxifier") {
        return {
            {"<T>", R"((\d+)\.(\d+) (\d+):(\d+):(\d+)(?:\.(\d+))?)"}
        };
    } else if (logname == "Spark") {
        return {
            {"<I>", R"((\d+)\.(\d+)\.(\d+)\.(\d+))"},
            {"<T>", R"((\d{2})\/(\d{2})\/(\d{2}) (\d+):(\d+):(\d+))"},
            {"<M>", R"((\d+)\.(\d{1}) MB)"},
            {"<K>", R"((\d+)\.(\d{1}) KB)"},
            {"<G>", R"((\d+)\.(\d{1}) GB)"},
            {"<B>", R"((\d+)\.(\d{1}) B)"}
        };
    } else if (logname == "Thunderbird") {
        return {
            {"<I>", R"((\d+)\.(\d+)\.(\d+)\.(\d+))"},
            {"<T>", R"((\d+):(\d+):(\d+))"},
            {"<A>", R"((\d{4})\.(\d+)\.(\d+))"},
            {"<B>", R"(\[(\d+)\]:)"}
        };
    } else if (logname == "Windows") {
        return {
            {"<I>", R"((\d+)\.(\d+)\.(\d+)\.(\d+))"},
            {"<T>", R"((\d+)-(\d+)-(\d+) (\d+):(\d+):(\d+))"},
            {"<D>", R"((\d+):(\d+):(\d+))"}
        };
    } else if (logname == "Zookeeper") {
        return {
            {"<I>", R"((\d+)\.(\d+)\.(\d+)\.(\d+))"},
            {"<T>", R"((\d+)-(\d+)-(\d+) (\d+):(\d+):(\d+),(\d+))"},
            {"<D>", R"((\d+):(\d+):(\d+))"}
        };
    }
    return {};
}

// 格式化数值为对应占位符的字符串
std::vector<std::string> format_numbers_by_placeholder(const std::string& logname, const std::string& placeholder, const std::vector<int64_t>& numbers) {
    std::vector<std::string> result;
    if (logname == "Apache" && placeholder == "<T>") {
        // Apache <T>: (\d{2}) (\d+):(\d+):(\d+)
        // 例：9060705 -> 09 06:07:05
        for (auto num : numbers) {
            int hms = num % 1000000;
            int hour = hms / 10000;
            int min = (hms / 100) % 100;
            int sec = hms % 100;
            int day = num / 1000000;
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d %02d:%02d:%02d", day, hour, min, sec);
            result.push_back(std::string(buf));
        }
    } else if (logname == "Apache" && placeholder == "<I>") {
        // Apache <I>: (\d+).(\d+).(\d+).(\d+)
        // 例：20410020022 -> 204.100.200.22（每段三位，不足补零）
        for (auto num : numbers) {
            int segs[4];
            segs[3] = num % 1000;
            num /= 1000;
            segs[2] = num % 1000;
            num /= 1000;
            segs[1] = num % 1000;
            num /= 1000;
            segs[0] = num;
            char buf[32];
            snprintf(buf, sizeof(buf), "%d.%d.%d.%d", segs[0], segs[1], segs[2], segs[3]);
            result.push_back(std::string(buf));
        }
    }
    // 可按需扩展其它 logname/placeholder 的格式化
    else {
        // 默认直接转字符串
        for (auto num : numbers) result.push_back(std::to_string(num));
    }
    return result;
}

// 还原单个 block
void restore_block(const std::string& block_dir, const std::string& logname) {
    
    // 获取当前 logname 的占位符与格式对应关系
    auto placeholder_formats = get_placeholder_formats(logname);
    /*std::cout << "[占位符格式映射] logname: " << logname << std::endl;
    for (const auto& kv : placeholder_formats) {
        std::cout << "  " << kv.first << " : " << kv.second << std::endl;
    }*/
    

    // 1. 读取模板和变量内容
    std::vector<std::string> templates, variables;
    std::vector<int> template_ids, variable_ids;
    std::string template_path = block_dir + logname + "allmapping.txt";
    std::string template_id_path = block_dir + logname + "allids.bin";
    std::string variable_path = block_dir + logname + "variablesetmapping.txt";
    std::string variable_id_path = block_dir + logname + "variablesetids.bin";

    if (!fs::exists(template_path) || !fs::exists(template_id_path)) return;
    templates = read_mapping(template_path);
    template_ids = read_ids(template_id_path);

    if (fs::exists(variable_path) && fs::exists(variable_id_path)) {
        variables = read_mapping(variable_path);
        variable_ids = read_ids(variable_id_path);
    }

    // 2. 用模板 ID 还原日志模板
    std::vector<std::string> block_logs;
    for (auto id : template_ids) {
        if (id <= 0 || id > (int)templates.size()) block_logs.push_back("");
        else block_logs.push_back(templates[id-1]);
    }

    // 3. 用变量 ID 还原变量内容，并替换 <*>
    std::vector<std::string> var_replacements;
    for (auto id : variable_ids) {
        if (id > 0 && id <= (int)variables.size()) var_replacements.push_back(variables[id-1]);
        else var_replacements.push_back("");
    }
    replace_placeholders(block_logs, "<*>", var_replacements);

    // 4. 查找所有 *.bin 文件，替换特殊标签
    auto bin_files = find_bin_files(block_dir);
    for (const auto& bin_path : bin_files) {
        std::string fname = bin_path.filename().string();
        // 解析标签名
        std::smatch m;
        std::regex pat(R"(_([a-zA-Z]+)_\.bin)");
        if (std::regex_search(fname, m, pat)) {
            std::string tag = m[1];
            std::string placeholder = "<" + tag + ">";
            // *.bin 文件存储了特殊标签对应的数字序列
            // 对于 <I>, <a>, <b>，直接解码即得到数字序列
            // 对于其他标签，解码后做逆 delta 还原，才得到真实的数字序列
            auto nums = read_bin_numbers(bin_path.string(), !(tag == "I" || tag == "a" || tag == "b" || tag == "c"));
            std::vector<int64_t> replacements(nums.begin(), nums.end());
            // 如果有格式映射，则格式化数值后替换占位符，否则用数字替换
            if (placeholder_formats.count(placeholder)) {
                auto formatted = format_numbers_by_placeholder(logname, placeholder, replacements);
                replace_placeholders(block_logs, placeholder, formatted);
            } else {
                replace_placeholders_num(block_logs, placeholder, replacements);
            }
        }
    }

    // 5. 输出还原后的日志
    std::string tmp_log_path=block_dir+"decompressed.log";
    std::ofstream tmp_log_file(tmp_log_path);
    for(const auto& line:block_logs){
        if(line.empty()) continue;
        tmp_log_file<<line<<"\n";
    }
    tmp_log_file.close();
}
class AutoWorkers{
public:
  explicit AutoWorkers(uint32_t maxw)
  :max_workers(maxw) {
    free_ids.reserve(max_workers);
    for (uint32_t i = 0; i < max_workers; ++i) free_ids.push_back(i);
  }
  void process_block(int block_id,const std::string& logname) {
    uint32_t wid = worker_id();
    std::string xzfile = "output/" + logname + "/compressed" + std::to_string(block_id) + ".tar.xz";
    std::string block_dir = "tmp_decompress/output/" + logname + "/" + std::to_string(block_id) + "/";
    decompress_xz(xzfile, "tmp_decompress");

    if (!fs::exists(block_dir)) {
        std::cerr << "  [ERROR] Block directory does not exist after decompress: " << block_dir << std::endl;
        return;
    }
    restore_block(block_dir, logname);
    std::cout << "Finished processing block: " << block_id << std::endl;
  }
private:
  class ThreadSlot{
    public:
      ThreadSlot(AutoWorkers* _owner):owner(_owner), id(owner->acquire()){}
      ~ThreadSlot(){owner->release(id);}
      uint32_t getid()const{return id;}
    private:
      AutoWorkers* owner;
      uint32_t id;
  };
  uint32_t worker_id() {
    thread_local ThreadSlot slot(this);
    return slot.getid();
  }
  uint32_t acquire(){
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&]{ return !free_ids.empty(); });
    uint32_t id = free_ids.back();
    free_ids.pop_back();
    return id;
  }
  void release(uint32_t id) {
    std::lock_guard<std::mutex> g(mu);
    free_ids.push_back(id);
    cv.notify_one();
  }
  uint32_t max_workers;
  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint32_t> free_ids;
};
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <logname>" << std::endl;
        return 1;
    }
    std::string logname = argv[1];
    std::string outdir = "output/" + logname;
    std::string tmpdir = "tmp_decompress/output/" + logname;
    std::string command = "rm -rf " + tmpdir;;
    int result = std::system(command.c_str());
    fs::create_directories(tmpdir);

    auto start = std::chrono::high_resolution_clock::now();

    auto block_ids = find_block_ids(outdir);
    std::cout << "Found block ids: ";
    for (auto id : block_ids) std::cout << id << " ";
    std::cout << std::endl;
    
    const int num_threads = 32; // Adjust based on the number of CPU cores.(初始为4)
    std::vector<std::future<void>> futures;
    AutoWorkers auto_workers(num_threads);
    for (int block_id : block_ids) {
        futures.push_back(
            std::async(
                std::launch::async, 
                &AutoWorkers::process_block, 
                &auto_workers, 
                block_id, 
                logname
            )
        );
    }
    for (auto& future : futures) { // Wait for all threads to complete and rethrow any exceptions.
        try {
            future.get();
        } catch (const std::exception &e) {
            std::cerr << "Worker thread threw exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Worker thread threw unknown exception." << std::endl;
        }
    }
    futures.clear();
    
    std::string out_log_path = outdir + "_decompressed.log";
    std::ofstream fout(out_log_path);
    for(int block_id : block_ids){
        std::string block_dir = tmpdir + "/" + std::to_string(block_id) + "/";
        std::string tmp_log_path=block_dir + "decompressed.log";
        std::ifstream fin(tmp_log_path);
        if(!fin.is_open()){
            std::cerr<<"[Warning] Cannot open decompressed log file for block "<<block_id<<std::endl;
            continue;
        }
        std::string line;
        while(std::getline(fin,line)){
            if(line.empty()) continue;
            fout<<line<<"\n";
        }
        fin.close();
    }
    fout.close();

    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    std::ofstream info_file("info.txt",std::ios::app);
    info_file << "decompress_denumsindex" << std::endl;
    info_file << "Logname: " << logname << std::endl;
    info_file << "Decompression completed in " << duration.count() << " milliseconds." << std::endl;
    info_file << "Duration: " << std::setprecision(6) << duration.count()/1000.0 << " seconds" << std::endl;
    info_file << "----------------------------------------" << std::endl;
    info_file.close();
    std::cout << "Decompressed log saved to: " << out_log_path << std::endl;
    return 0;
}
//g++ -std=c++17 -o decompress_denum decompress_denum.cpp
//./decompress_denum Apache