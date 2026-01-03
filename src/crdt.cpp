#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <iomanip>
#include <ctime>
#include <thread>
#include <chrono>
#include <cmath>

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    #include <unistd.h>
    #include <sys/stat.h>
#endif

using namespace std;

// --- Configuration ---
const string REGISTRY_FILE = "registry.json";
const string INITIAL_DOC_FILE = "initial_document.txt";
const int MONITOR_INTERVAL_MS = 2000; // 2 seconds

// --- CRDT Operation Structure ---
struct CRDTOperation {
    string type;
    string userId;
    long long timestamp;
    int lineNumber;
    int startCol;
    int endCol;
    string oldContent;
    string newContent;
};

// --- Utility: Registry (Simulated Shared Memory) Functions ---

// Note: For simplicity and to avoid external JSON libraries, the registry
// is managed as a simple text/map structure.

struct UserInfo {
    int pid = 0;
    string queueName;
    double lastSeen = 0.0;
};

map<string, UserInfo> load_registry() {
    map<string, UserInfo> registry;
    ifstream ifs(REGISTRY_FILE);
    if (!ifs.is_open()) {
        return registry;
    }
    
    // Simplistic parsing of the JSON structure (only key/value lines)
    string line;
    string current_user_id;

    while (getline(ifs, line)) {
        line.erase(0, line.find_first_not_of(" \t")); // Trim leading whitespace
        
        if (line.empty()) continue;

        if (line.find('"') == 0) {
            // Check for user ID start: e.g., "user_1": {
            size_t id_end = line.find(':');
            if (id_end != string::npos) {
                current_user_id = line.substr(1, id_end - 2); // Extract user ID
                if (registry.find(current_user_id) == registry.end()) {
                    registry[current_user_id] = UserInfo();
                }
            }
        }
        
        if (current_user_id.empty()) continue;

        size_t colon_pos = line.find(':');
        if (colon_pos != string::npos) {
            string key = line.substr(0, colon_pos);
            size_t value_start = colon_pos + 1;
            string value_str = line.substr(value_start);

            // Clean up key (remove quotes and whitespace)
            key.erase(0, key.find_first_not_of(" \t\""));
            key.erase(key.find_last_not_of(" \t\"") + 1);

            // Clean up value (remove quotes, comma, bracket, and whitespace)
            value_str.erase(value_str.find_last_not_of(" \t,\"} ]") + 1);
            value_str.erase(0, value_str.find_first_not_of(" \t\""));

            if (key == "pid") {
                try { registry[current_user_id].pid = stoi(value_str); } catch(...) {}
            } else if (key == "queue_name") {
                registry[current_user_id].queueName = value_str;
            } else if (key == "last_seen") {
                try { registry[current_user_id].lastSeen = stod(value_str); } catch(...) {}
            }
        }
    }
    return registry;
}

void save_registry(const map<string, UserInfo>& registry) {
    ofstream ofs(REGISTRY_FILE);
    if (!ofs.is_open()) return;

    ofs << "{" << endl;
    bool first_user = true;
    for (const auto& pair : registry) {
        if (!first_user) {
            ofs << "," << endl;
        }
        ofs << "    \"" << pair.first << "\": {" << endl;
        ofs << "        \"pid\": " << pair.second.pid << "," << endl;
        ofs << "        \"queue_name\": \"" << pair.second.queueName << "\"," << endl;
        ofs << "        \"last_seen\": " << fixed << setprecision(6) << pair.second.lastSeen << endl;
        ofs << "    }";
        first_user = false;
    }
    ofs << endl << "}" << endl;
}

void register_user(const string& user_id) {
    auto registry = load_registry();
    
    UserInfo info;
    info.pid = static_cast<int>(getpid());
    info.queueName = "queue_" + user_id;
    info.lastSeen = static_cast<double>(time(nullptr));
    
    registry[user_id] = info;
    save_registry(registry);
    cout << "User " << user_id << " registered successfully." << endl;
}

vector<string> discover_users(const string& current_user_id) {
    auto registry = load_registry();
    vector<string> active_users;
    double current_time = static_cast<double>(time(nullptr));
    
    for (const auto& pair : registry) {
        // Mark active if seen recently (5 times the monitoring interval)
        if (current_time - pair.second.lastSeen < (MONITOR_INTERVAL_MS / 1000.0) * 5) {
            active_users.push_back(pair.first);
        }
    }
    sort(active_users.begin(), active_users.end()); // Keep order consistent
    return active_users;
}

// --- Document Management ---

void initialize_local_document(const string& user_doc_path) {
    if (!ifstream(INITIAL_DOC_FILE).is_open()) {
        throw runtime_error("Missing initial document: " + INITIAL_DOC_FILE + ". Please create it.");
    }
    if (!ifstream(user_doc_path).is_open()) {
        ifstream src(INITIAL_DOC_FILE, ios::binary);
        ofstream dst(user_doc_path, ios::binary);
        dst << src.rdbuf();
        cout << "Created local document: " << user_doc_path << endl;
    }
}

vector<string> load_document_content(const string& path) {
    vector<string> lines;
    ifstream file(path);
    if (file.is_open()) {
        string line;
        while (getline(file, line)) {
            // Remove carriage return if present (for cross-OS consistency)
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(line);
        }
        file.close();
    }
    return lines;
}

long long get_file_mtime(const string& path) {
    struct stat result;
    if (stat(path.c_str(), &result) == 0) {
        // Return modification time in seconds (POSIX standard)
        return result.st_mtime; 
    }
    return 0;
}

// --- Change Detection Logic ---

vector<CRDTOperation> find_changes(const string& user_id, const vector<string>& old_lines, const vector<string>& new_lines) {
    vector<CRDTOperation> updates;
    size_t max_len = max(old_lines.size(), new_lines.size());
    // Get timestamp in milliseconds for finer granularity (CRDT requirement)
    long long timestamp = chrono::duration_cast<chrono::milliseconds>(
                            chrono::system_clock::now().time_since_epoch()).count();

    for (size_t line_num = 0; line_num < max_len; ++line_num) {
        string old_line = (line_num < old_lines.size()) ? old_lines[line_num] : "";
        string new_line = (line_num < new_lines.size()) ? new_lines[line_num] : "";

        if (old_line == new_line) {
            continue;
        }

        // --- Change Identification for a single line ---
        
        // 1. Find the start of the difference
        size_t start_col = 0;
        while (start_col < old_line.length() && start_col < new_line.length() && old_line[start_col] == new_line[start_col]) {
            start_col++;
        }

        // 2. Find the end of the difference (from the right)
        size_t old_end_offset = 0;
        size_t new_end_offset = 0;

        while ((old_line.length() - 1 - old_end_offset) >= start_col &&
               (new_line.length() - 1 - new_end_offset) >= start_col &&
               old_line[old_line.length() - 1 - old_end_offset] == new_line[new_line.length() - 1 - new_end_offset]) {
            old_end_offset++;
            new_end_offset++;
        }

        size_t old_change_end = old_line.length() - old_end_offset;
        size_t new_change_end = new_line.length() - new_end_offset;

        // Content removed (from old line)
        string content_removed = old_line.substr(start_col, old_change_end - start_col);
        // Content added (to new line)
        string content_added = new_line.substr(start_col, new_change_end - start_col);

        // Determine Operation Type
        string op_type = "REPLACE";
        if (content_removed.empty() && !content_added.empty()) {
            op_type = "INSERT";
        } else if (!content_removed.empty() && content_added.empty()) {
            op_type = "DELETE";
        } 
        
        // The column range for the CRDT operation object
        int end_col = static_cast<int>(start_col + content_removed.length());

        CRDTOperation op = {
            op_type,
            user_id,
            timestamp,
            static_cast<int>(line_num),
            static_cast<int>(start_col),
            end_col,
            content_removed,
            content_added,
        };
        updates.push_back(op);
    }

    return updates;
}

// --- Terminal Display ---

// Helper function to serialize CRDTOperation to a JSON-like string
string op_to_json(const CRDTOperation& op) {
    ostringstream oss;
    oss << "{\"type\":\"" << op.type << "\", \"userId\":\"" << op.userId << "\", \"timestamp\":" << op.timestamp
        << ", \"lineNumber\":" << op.lineNumber << ", \"startCol\":" << op.startCol << ", \"endCol\":" << op.endCol
        << ", \"oldContent\":\"" << op.oldContent << "\", \"newContent\":\"" << op.newContent << "\"}";
    return oss.str();
}

void display_terminal(const string& user_doc_path, const vector<string>& current_content, 
                      const vector<string>& active_users, const vector<CRDTOperation>& updates) {
    
    // Clear the terminal screen using ANSI escape codes
    cout << "\033[2J\033[H";
    
    // Get current time string
    time_t rawtime;
    struct tm * timeinfo;
    char buffer[80];
    time (&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buffer, sizeof(buffer), "%H:%M:%S", timeinfo);

    cout << "Document: " << user_doc_path << endl;
    cout << "Last updated: " << buffer << endl;
    cout << string(40, '-') << endl;
    
    // Display Document Content
    vector<int> modified_lines;
    for (const auto& update : updates) {
        modified_lines.push_back(update.lineNumber);
    }
    
    for (size_t i = 0; i < current_content.size(); ++i) {
        cout << "Line " << i << ": " << current_content[i];
        if (find(modified_lines.begin(), modified_lines.end(), (int)i) != modified_lines.end()) {
            cout << " [MODIFIED]";
        }
        cout << endl;
    }

    cout << string(40, '-') << endl;
    
    // Display Status and Active Users
    cout << "Active users: ";
    for (size_t i = 0; i < active_users.size(); ++i) {
        cout << active_users[i] << (i < active_users.size() - 1 ? ", " : "");
    }
    cout << endl;
    cout << "Monitoring for changes..." << endl;

    // Display Detected Changes
    for (const auto& update : updates) {
        string change_desc;
        if (update.type == "INSERT") {
            change_desc = "Inserted '" + update.newContent + "'";
        } else if (update.type == "DELETE") {
            change_desc = "Removed '" + update.oldContent + "'";
        } else { // REPLACE
            change_desc = "Replaced '" + update.oldContent + "' with '" + update.newContent + "'";
        }

        cout << "--> Change detected (Op: " << update.type << "): Line " << update.lineNumber
                  << ", column " << update.startCol << ", " << change_desc << endl;
        cout << "    CRDT Update Object: " << op_to_json(update) << endl;
    }
}


// --- Main Application Loop ---

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: ./editor <user_id>" << endl;
        return 1;
    }

    string user_id = argv[1];
    string user_doc_path = user_id + "_doc.txt";

    try {
        register_user(user_id);
        initialize_local_document(user_doc_path);
    } catch (const exception& e) {
        cerr << "Initialization Error: " << e.what() << endl;
        return 1;
    }

    // Initial state tracking
    long long last_mtime = get_file_mtime(user_doc_path);
    vector<string> last_content = load_document_content(user_doc_path);

    // Display initial state
    vector<string> active_users = discover_users(user_id);
    display_terminal(user_doc_path, last_content, active_users, {});

    while (true) {
        try {
            // Sleep for the monitoring interval
            this_thread::sleep_for(chrono::milliseconds(MONITOR_INTERVAL_MS));
            
            // 1. File Monitoring (Check modification time)
            long long current_mtime = get_file_mtime(user_doc_path);
            
            vector<CRDTOperation> updates;
            vector<string> current_content = last_content; // Assume no change initially

            if (current_mtime != last_mtime) {
                // Modification detected!
                current_content = load_document_content(user_doc_path);
                
                // 2. Change Identification
                updates = find_changes(user_id, last_content, current_content);

                // 3. Update state
                last_mtime = current_mtime;
                last_content = current_content; 
            }

            // Update last_seen in registry
            auto registry = load_registry();
            if (registry.count(user_id)) {
                registry[user_id].lastSeen = static_cast<double>(time(nullptr));
                save_registry(registry);
            }

            // 4. Terminal Display
            active_users = discover_users(user_id);
            display_terminal(user_doc_path, current_content, active_users, updates);

        } catch (const exception& e) {
            cerr << "\nAn error occurred: " << e.what() << endl;
            // Optionally wait longer before retrying
            this_thread::sleep_for(chrono::seconds(5)); 
        }
    }

    return 0;
}