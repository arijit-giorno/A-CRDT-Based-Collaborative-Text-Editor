// SyncText (Lock-Free) — Part 1–3: registry+monitor+MQ broadcast + Matrix LWW CRDT
// Build: g++ -std=gnu++17 -O2 -pthread editor.cpp -o editor -lrt
// Run  : ./editor user_1   (similarly user_2, user_3, ... on Ubuntu 22+)

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <climits>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mqueue.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__) || defined(_WIN32)
#error "This uses POSIX message queues. Run on Linux."
#endif
using namespace std;

// ----------------- Config -----------------
const string REGISTRY_FILE = "registry.json";
const string INITIAL_DOC_FILE = "initial_document.txt";
const int    MONITOR_INTERVAL_MS = 1000;       // 1s scan
const size_t BROADCAST_BATCH_SIZE = 5;         // send after 5 local ops
const size_t MERGE_THRESHOLD      = 5;         // merge after >=5 remote ops
const int    MERGE_MAX_MS         = 4000;      // or every 4s if anything pending
#define MAX_MSG_SIZE 4096
#define MAX_MSG_COUNT 10

// ----------------- Types ------------------
struct CRDTOperation {
    string type;          // always REPLACE in this build
    string userId;
    long long timestamp;  // ms epoch
    int lineNumber;
    int startCol;
    int endCol;
    string oldContent;
    string newContent;

    string to_string() const {
        return type + " L" + std::to_string(lineNumber) + ":" +
               std::to_string(startCol) + "-" + std::to_string(endCol) +
               " '" + oldContent + "'->'" + newContent + "'";
    }
};

struct UserInfo {
    int pid = 0;
    string queueName;
    double lastSeen = 0.0;
};

// ----------------- Time/FS utils -----------------
static inline long long now_ms() {
    return chrono::duration_cast<chrono::milliseconds>(
               chrono::system_clock::now().time_since_epoch()).count();
}

long long get_file_mtime(const string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) return st.st_mtime;
    return 0;
}

vector<string> load_document_content(const string& path) {
    vector<string> lines;
    ifstream f(path);
    string line;
    while (getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

void write_document_content(const string& path, const vector<string>& lines) {
    ofstream out(path);
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << "\n";
    }
}

void initialize_local_document(const string& user_doc_path) {
    if (!ifstream(INITIAL_DOC_FILE).good()) {
        throw runtime_error("Missing '" + INITIAL_DOC_FILE + "'. Create it first.");
    }
    if (!ifstream(user_doc_path).good()) {
        ifstream src(INITIAL_DOC_FILE, ios::binary);
        ofstream dst(user_doc_path, ios::binary);
        dst << src.rdbuf();
        cout << "Created local document: " << user_doc_path << endl;
    }
}

// --------------- Registry (tiny JSON-ish) ---------------
map<string, UserInfo> load_registry() {
    map<string, UserInfo> reg;
    ifstream in(REGISTRY_FILE);
    if (!in) return reg;

    string line, currentUser;
    UserInfo ui;
    bool insideUser = false;

    while (getline(in, line)) {
        line.erase(0, line.find_first_not_of(" \t\n\r"));
        if (line.empty()) continue;
        line.erase(line.find_last_not_of(" \t\n\r") + 1);

        if (line[0] == '"' && line.find("\": {") != string::npos) {
            size_t pos1 = line.find('"') + 1;
            size_t pos2 = line.find('"', pos1);
            currentUser = line.substr(pos1, pos2 - pos1);
            ui = UserInfo(); insideUser = true; continue;
        }
        if (!insideUser) continue;

        if (line.find("}") != string::npos) {
            reg[currentUser] = ui; insideUser = false; continue;
        }
        if (line.find("\"pid\"") != string::npos) {
            ui.pid = stoi(line.substr(line.find(':') + 1));
        } else if (line.find("\"queue_name\"") != string::npos) {
            size_t q1 = line.find('"', line.find(':') + 1) + 1;
            size_t q2 = line.find('"', q1);
            ui.queueName = line.substr(q1, q2 - q1);
        } else if (line.find("\"last_seen\"") != string::npos) {
            ui.lastSeen = stod(line.substr(line.find(':') + 1));
        }
    }
    return reg;
}

void save_registry(const map<string, UserInfo>& reg) {
    ofstream out(REGISTRY_FILE);
    out << "{\n";
    bool first = true;
    for (auto& [uid, ui] : reg) {
        if (!first) out << ",\n";
        first = false;
        out << "  \"" << uid << "\": {\n"
            << "    \"pid\": " << ui.pid << ",\n"
            << "    \"queue_name\": \"" << ui.queueName << "\",\n"
            << "    \"last_seen\": " << fixed << setprecision(6) << ui.lastSeen << "\n"
            << "  }";
    }
    out << "\n}\n";
}

void register_user(const string& user_id) {
    auto reg = load_registry();
    UserInfo ui;
    ui.pid = static_cast<int>(getpid());
    ui.queueName = "/queue_" + user_id;
    ui.lastSeen = time(nullptr);
    reg[user_id] = ui;
    save_registry(reg);
    cout << "Registered " << user_id << " -> " << ui.queueName << endl;
}

vector<string> discover_users() {
    auto reg = load_registry();
    vector<string> v;
    double t = time(nullptr);
    for (auto& [uid, ui] : reg) {
        if (t - ui.lastSeen < (MONITOR_INTERVAL_MS / 1000.0) * 5.0) v.push_back(uid);
    }
    sort(v.begin(), v.end());
    return v;
}

// --------------- Op (safe JSON-ish) ---------------
static inline string escape_json(const string& s) {
    string out; out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

string op_to_json(const CRDTOperation& op) {
    ostringstream oss;
    oss << "{"
        << "\"type\":\"" << escape_json(op.type) << "\","
        << "\"userId\":\"" << escape_json(op.userId) << "\","
        << "\"timestamp\":" << op.timestamp << ","
        << "\"lineNumber\":" << op.lineNumber << ","
        << "\"startCol\":" << op.startCol << ","
        << "\"endCol\":" << op.endCol << ","
        << "\"oldContent\":\"" << escape_json(op.oldContent) << "\","
        << "\"newContent\":\"" << escape_json(op.newContent) << "\""
        << "}";
    return oss.str();
}

static bool read_json_string(const string& s, size_t& i, string& out) {
    ++i; out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '\\') {
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
            case '\\': out+='\\'; break;
            case '"':  out+='"';  break;
            case 'n':  out+='\n'; break;
            case 'r':  out+='\r'; break;
            case 't':  out+='\t'; break;
            default:   out+=e;    break;
            }
        } else if (c == '"') return true;
        else out += c;
    }
    return false;
}

CRDTOperation json_to_op(const string& s) {
    CRDTOperation op;
    size_t i = 0;
    auto skip = [&](){ while (i < s.size() && isspace((unsigned char)s[i])) ++i; };
    auto expect = [&](char ch){ skip(); if (i<s.size() && s[i]==ch){++i; return true;} return false; };
    expect('{');
    while (i < s.size()) {
        skip(); if (i>=s.size() || s[i]=='}') break;
        if (s[i]!='"') break;
        string key; read_json_string(s, i, key);
        expect(':'); skip();
        if (key=="type"||key=="userId"||key=="oldContent"||key=="newContent") {
            if (s[i]!='"') break;
            string val; read_json_string(s, i, val);
            if      (key=="type")       op.type = val;
            else if (key=="userId")     op.userId = val;
            else if (key=="oldContent") op.oldContent = val;
            else                        op.newContent = val;
        } else {
            size_t j = i;
            while (j < s.size() && (s[j]=='-'||s[j]=='+'||s[j]=='.'||isdigit((unsigned char)s[j]))) ++j;
            string num = s.substr(i, j-i);
            if      (key=="timestamp")  op.timestamp = atoll(num.c_str());
            else if (key=="lineNumber") op.lineNumber = atoi(num.c_str());
            else if (key=="startCol")   op.startCol = atoi(num.c_str());
            else if (key=="endCol")     op.endCol = atoi(num.c_str());
            i = j;
        }
        skip(); if (i < s.size() && s[i]==',') ++i;
    }
    return op;
}

// --------------- Change detection (line-wise) ---------------
// Always emit full-line ops so merges have entire row state.
vector<CRDTOperation> find_changes(const string& user_id,
                                   const vector<string>& old_lines,
                                   const vector<string>& new_lines) {
    vector<CRDTOperation> updates;
    size_t max_len = max(old_lines.size(), new_lines.size());
    for (size_t ln = 0; ln < max_len; ++ln) {
        string oldL = (ln < old_lines.size()) ? old_lines[ln] : "";
        string newL = (ln < new_lines.size()) ? new_lines[ln] : "";
        if (oldL == newL) continue;

        CRDTOperation op;
        op.type       = "REPLACE";
        op.userId     = user_id;
        op.timestamp  = now_ms();
        op.lineNumber = static_cast<int>(ln);
        op.startCol   = 0;
        op.endCol     = (int)oldL.size();
        op.oldContent = oldL;
        op.newContent = newL; // FULL LINE
        updates.push_back(op);
    }
    return updates;
}

// --------------- Lock-free SPSC ring buffer ---------------
template <typename T, size_t Capacity>
struct SPSC {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");
    T buf[Capacity];
    atomic<size_t> head{0}; // producer writes at head
    atomic<size_t> tail{0}; // consumer reads at tail

    bool push(const T& v) {
        size_t h = head.load(memory_order_relaxed);
        size_t t = tail.load(memory_order_acquire);
        if (((h + 1) & (Capacity - 1)) == (t & (Capacity - 1))) return false; // full
        buf[h & (Capacity - 1)] = v;
        head.store(h + 1, memory_order_release);
        return true;
    }
    bool pop(T& out) {
        size_t t = tail.load(memory_order_relaxed);
        size_t h = head.load(memory_order_acquire);
        if (t == h) return false; // empty
        out = buf[t & (Capacity - 1)];
        tail.store(t + 1, memory_order_release);
        return true;
    }
    size_t size() const {
        size_t h = head.load(memory_order_acquire);
        size_t t = tail.load(memory_order_acquire);
        return (h - t);
    }
};

// --------------- MQ utilities ---------------
void ensure_own_queue_exists(const string& user_id) {
    string qname = "/queue_" + user_id;
    struct mq_attr attr{};
    attr.mq_flags = 0;
    attr.mq_maxmsg = MAX_MSG_COUNT;
    attr.mq_msgsize = MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;

    mqd_t mq = mq_open(qname.c_str(), O_CREAT | O_RDONLY | O_NONBLOCK, 0644, &attr);
    if (mq == (mqd_t)-1) {
        cerr << "FATAL: cannot open own MQ " << qname << " errno=" << errno << endl;
        exit(2);
    }
    mq_close(mq);
}

void send_update(const string& queue_name, const string& json) {
    mqd_t mq = mq_open(queue_name.c_str(), O_WRONLY | O_NONBLOCK);
    if (mq == (mqd_t)-1) return; // dest not up yet
    size_t n = json.size();
    if (n > MAX_MSG_SIZE) n = MAX_MSG_SIZE;
    if (mq_send(mq, json.data(), n, 0) == -1) {
        // queue full / not available — drop (lock-free design)
    }
    mq_close(mq);
}

void broadcast_updates(const string& self, const vector<CRDTOperation>& updates) {
    if (updates.empty()) return;
    auto reg = load_registry();
    for (const auto& op : updates) {
        string payload = op_to_json(op);
        for (const auto& [uid, ui] : reg) {
            if (uid == self) continue;
            if (!ui.queueName.empty() && ui.queueName[0] == '/')
                send_update(ui.queueName, payload);
        }
    }
}

// --------------- Helpers ---------------
static inline string join(const vector<string>& v, const string& sep) {
    ostringstream oss;
    for (size_t i=0;i<v.size();++i){ if(i)oss<<sep; oss<<v[i]; }
    return oss.str();
}

// Keep for UI; not used by matrix merge.
static void apply_operation_to_line(string& line, const CRDTOperation& op) {
    int start = max(0, min(op.startCol, (int)line.size()));
    int end   = max(start, min(op.endCol, (int)line.size()));
    string prefix = line.substr(0, start);
    string suffix = line.substr(end);
    line = prefix + op.newContent + suffix;
}

static inline bool newer_than(const CRDTOperation& a, const CRDTOperation& b) {
    if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
    return a.userId < b.userId; // stable tie-break
}

// -------- Matrix merge: per-column LWW, spaces treated as empty ----------
static void merge_per_line_MATRIX(vector<string>& base_lines,
                                  const vector<CRDTOperation>& local_since_merge,
                                  const vector<CRDTOperation>& remote_ops)
{
    // combine
    vector<CRDTOperation> all = local_since_merge;
    all.insert(all.end(), remote_ops.begin(), remote_ops.end());
    if (all.empty()) return;

    // group by line -> latest op per userId
    map<int, map<string, CRDTOperation>> latest_by_line_user;
    for (const auto& op : all) {
        int ln = op.lineNumber;
        auto& by_user = latest_by_line_user[ln];
        auto it = by_user.find(op.userId);
        if (it == by_user.end() || newer_than(op, it->second)) by_user[op.userId] = op;
    }

    for (auto& [ln, by_user] : latest_by_line_user) {
        if (ln >= (int)base_lines.size()) base_lines.resize(ln + 1);
        // baseline (spaces) has implicit timestamp = very old
        size_t maxlen = base_lines[ln].size();
        for (auto& [_,op] : by_user) maxlen = max(maxlen, op.newContent.size());
        string merged(maxlen, ' ');

        for (size_t c = 0; c < maxlen; ++c) {
            char best_ch = ' ';
            long long best_ts = LLONG_MIN;
            string best_uid = "~"; // after 'z'

            // consider current file view as a source with ts=LLONG_MIN+1 (preserve if nobody writes non-space)
            if (c < base_lines[ln].size() && base_lines[ln][c] != ' ') {
                best_ch = base_lines[ln][c];
                best_ts = LLONG_MIN + 1;
                best_uid = "zzzzzz";
            }

            for (const auto& [uid, op] : by_user) {
                char ch = (c < op.newContent.size()) ? op.newContent[c] : ' ';
                if (ch == ' ') continue; // empty cell
                if (op.timestamp > best_ts ||
                   (op.timestamp == best_ts && uid < best_uid)) {
                    best_ch = ch;
                    best_ts = op.timestamp;
                    best_uid = uid;
                }
            }
            merged[c] = best_ch;
        }

        // rtrim trailing spaces (keeps interior spaces)
        while (!merged.empty() && merged.back()==' ') merged.pop_back();
        base_lines[ln] = merged;
    }
}

// --------------- UI helpers ---------------
void display_terminal(const string& user_id,
                      const string& user_doc_path,
                      const vector<string>& current_content,
                      const vector<string>& active_users,
                      const vector<CRDTOperation>& local_cycle,
                      const vector<CRDTOperation>& remote_snapshot,
                      size_t outbuf_size,
                      const string& merge_status = "")
{
    cout << "\033[2J\033[H";
    time_t raw = time(nullptr);
    char tbuf[32]; strftime(tbuf, sizeof(tbuf), "%H:%M:%S", localtime(&raw));

    cout << "CRDT Editor (User: " << user_id << ")\n";
    cout << "Last Scan: " << tbuf << " | Active users (" << active_users.size() << "): "
         << (active_users.empty()?string("-"):join(active_users, ", ")) << "\n";
    if (!merge_status.empty()) {
        cout << "Merge Status: " << merge_status << "\n";
    }
    cout << string(78, '=') << "\n";

    cout << "DOCUMENT: " << user_doc_path << "\n";
    for (size_t i = 0; i < current_content.size(); ++i)
        cout << "Line " << i << ": " << current_content[i] << "\n";
    cout << string(78, '-') << "\n";

    cout << "LOCAL CHANGES (buffered/outgoing):\n";
    if (local_cycle.empty()) {
        cout << "  No local changes detected in this cycle.\n";
    } else {
        for (const auto& u : local_cycle) {
            cout << "  [LOCAL] " << u.type << " L" << u.lineNumber
                 << " cols " << u.startCol << "-" << u.endCol
                 << " | '" << u.oldContent << "' -> '" << u.newContent << "'\n";
        }
    }
    cout << "Buffered total (pending broadcast): " << outbuf_size
         << " | Broadcast threshold: " << BROADCAST_BATCH_SIZE << "\n";
    cout << string(78, '-') << "\n";

    cout << "REMOTE UPDATES (listener buffer snapshot):\n";
    if (remote_snapshot.empty())
        cout << "  Listener idle.\n";
    else {
        for (const auto& u : remote_snapshot)
            cout << "  [REMOTE] " << u.type << " L" << u.lineNumber
                 << " from " << u.userId
                 << " | '" << u.oldContent << "' -> '" << u.newContent << "'\n";
    }
    cout << string(78, '=') << "\n";
    cout << flush;
}

// ============== Global lock-free structures ==============
static SPSC<CRDTOperation, 1024> incoming_spsc; // listener -> main
static atomic<bool> running{true};

// --------------- Listener thread (lock-free) ---------------
void listener_thread(const string& user_id) {
    string qname = "/queue_" + user_id;
    struct mq_attr attr{};
    attr.mq_maxmsg = MAX_MSG_COUNT;
    attr.mq_msgsize = MAX_MSG_SIZE;

    mqd_t mq = mq_open(qname.c_str(), O_CREAT | O_RDONLY | O_NONBLOCK, 0644, &attr);
    if (mq == (mqd_t)-1) {
        cerr << "FATAL: Cannot open MQ " << qname << " errno=" << errno << endl;
        return;
    }
    vector<char> buf(MAX_MSG_SIZE);

    while (running.load(memory_order_relaxed)) {
        ssize_t n = mq_receive(mq, buf.data(), MAX_MSG_SIZE, nullptr);
        if (n >= 0) {
            string msg(buf.data(), static_cast<size_t>(n));
            CRDTOperation op = json_to_op(msg);
            if (!incoming_spsc.push(op)) {
                CRDTOperation tmp;
                incoming_spsc.pop(tmp);
                incoming_spsc.push(op);
            }
        } else {
            this_thread::sleep_for(chrono::milliseconds(60));
        }
    }
    mq_close(mq);
}

// --------------- Main ---------------
int main(int argc, char* argv[]) {
    if (argc != 2) { cerr << "Usage: ./editor <user_id>\n"; return 1; }
    string user_id = argv[1];
    string user_doc_path = user_id + "_doc.txt";

    signal(SIGINT, [](int){ running.store(false); });

    try {
        register_user(user_id);
        initialize_local_document(user_doc_path);
        ensure_own_queue_exists(user_id);
    } catch (const exception& e) {
        cerr << "Init error: " << e.what() << "\n";
        return 1;
    }

    thread(listener_thread, user_id).detach();

    long long last_mtime = get_file_mtime(user_doc_path);
    vector<string> last_content = load_document_content(user_doc_path);

    vector<string> active_users = discover_users();
    display_terminal(user_id, user_doc_path, last_content, active_users, {}, {}, 0);

    // main-thread-only buffers (no locks)
    vector<CRDTOperation> outgoing_updates_buffer;
    vector<CRDTOperation> local_since_merge;
    vector<CRDTOperation> pending_remote;          // to be merged
    vector<CRDTOperation> remote_display_snapshot; // to be shown immediately

    long long last_merge_ms = now_ms();
    string merge_status;

    while (running.load(memory_order_relaxed)) {
        try {
            this_thread::sleep_for(chrono::milliseconds(MONITOR_INTERVAL_MS));
            merge_status.clear();

            // --- Monitor file for local changes
            vector<CRDTOperation> cycle_updates;
            long long cur_mtime = get_file_mtime(user_doc_path);
            vector<string> current_content = last_content;

            if (cur_mtime != last_mtime) {
                current_content = load_document_content(user_doc_path);
                cycle_updates = find_changes(user_id, last_content, current_content);
                if (!cycle_updates.empty()) {
                    outgoing_updates_buffer.insert(outgoing_updates_buffer.end(),
                                                   cycle_updates.begin(), cycle_updates.end());
                    local_since_merge.insert(local_since_merge.end(),
                                             cycle_updates.begin(), cycle_updates.end());
                }
                last_mtime = cur_mtime;
                last_content = current_content;
            }

            // --- Broadcast local batch
            if (outgoing_updates_buffer.size() >= BROADCAST_BATCH_SIZE) {
                vector<CRDTOperation> to_send(
                    outgoing_updates_buffer.begin(),
                    outgoing_updates_buffer.begin() + BROADCAST_BATCH_SIZE);
                outgoing_updates_buffer.erase(outgoing_updates_buffer.begin(),
                                              outgoing_updates_buffer.begin() + BROADCAST_BATCH_SIZE);
                broadcast_updates(user_id, to_send);
                cout << "\n[LOCAL BROADCAST] " << to_send.size() << " ops sent.\n" << flush;
            }

            // --- Drain listener into both pending_remote & display snapshot
            {
                CRDTOperation got;
                size_t drained = 0;
                remote_display_snapshot.clear();
                while (incoming_spsc.pop(got)) {
                    if (got.userId != user_id) {
                        pending_remote.push_back(got);
                        remote_display_snapshot.push_back(got);
                    }
                    ++drained;
                    if (drained > 2048) break; // safety cap per tick
                }
            }

            // --- Merge trigger
            bool need_merge = false;
            if (pending_remote.size() >= MERGE_THRESHOLD) {
                need_merge = true;
                merge_status = "Threshold reached (remote ops: " + to_string(pending_remote.size()) + ")";
            }
            if (!pending_remote.empty() && (now_ms() - last_merge_ms) >= MERGE_MAX_MS) {
                need_merge = true;
                merge_status = "Timeout reached";
            }

            if (need_merge) {
                vector<string> new_lines = last_content;

                // Matrix LWW over ALL ops since previous merge
                set<string> remote_users;
                for (const auto& op : pending_remote) remote_users.insert(op.userId);
                cout << "\n[MERGE] remote_ops=" << pending_remote.size()
                     << " local_ops_since=" << local_since_merge.size()
                     << " users: ";
                for (const auto& u : remote_users) cout << u << " ";
                cout << "\n" << flush;

                merge_per_line_MATRIX(new_lines, local_since_merge, pending_remote);

                // Write merged content back
                write_document_content(user_doc_path, new_lines);

                // Update local state
                last_content = new_lines;
                last_mtime = get_file_mtime(user_doc_path);

                // Clear buffers
                pending_remote.clear();
                local_since_merge.clear();
                last_merge_ms = now_ms();

                cout << "[MERGE] Completed. Document updated.\n" << flush;
            }

            // --- Update registry heartbeat
            {
                auto reg = load_registry();
                if (reg.count(user_id)) {
                    reg[user_id].lastSeen = time(nullptr);
                    save_registry(reg);
                }
            }

            active_users = discover_users();

            // Show current snapshot
            display_terminal(user_id, user_doc_path, last_content, active_users,
                             cycle_updates, remote_display_snapshot,
                             outgoing_updates_buffer.size(), merge_status);

        } catch (const exception& e) {
            cerr << "Main loop error: " << e.what() << "\n";
            this_thread::sleep_for(chrono::seconds(2));
        }
    }

    cout << "Shutting down " << user_id << "...\n";
    return 0;
}
