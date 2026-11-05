#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <ctime>
#include <iomanip>
#include <algorithm>
#include <random>

using namespace std;

// ==========================================
// CONFIGURATION PARAMETERS
// ==========================================
struct Config {
    int numCpu = 4;
    string scheduler = "rr"; // "fcfs" or "rr"
    uint32_t quantumCycles = 5;
    uint32_t batchProcessFreq = 1;
    uint32_t minIns = 1000;
    uint32_t maxIns = 2000;
    uint32_t delaysPerExec = 0;
} config;

// ==========================================
// INSTRUCTION REPRESENTATION
// ==========================================
enum InstructionType {
    INS_PRINT,
    INS_DECLARE,
    INS_ADD,
    INS_SUBTRACT,
    INS_SLEEP,
    INS_FOR
};

struct Instruction {
    InstructionType type;
    string var1;
    string var2;
    string var3;
    uint16_t val1 = 0;
    uint16_t val2 = 0;
    bool var2IsVal = false;
    bool var3IsVal = false;
    uint8_t sleepTicks = 0;
    int repeats = 0;
    vector<Instruction> innerInstructions;
};

// ==========================================
// PROCESS CONTROL BLOCK (PCB)
// ==========================================
enum ProcessState {
    READY,
    RUNNING,
    WAITING_SLEEP,
    FINISHED
};

struct Process {
    int pid;
    string name;
    ProcessState state;
    vector<Instruction> flatInstructions; // Expanded instructions to execute
    int totalInstructions = 0;
    int currentInstructionIndex = 0;
    int assignedCore = -1;
    string startTime;

    // Process memory & output logs
    map<string, uint16_t> memory;
    vector<string> logs;
    uint8_t sleepRemaining = 0;

    Process(int id, string pName) : pid(id), name(pName), state(READY), currentInstructionIndex(0) {}
};

// ==========================================
// GLOBAL SYSTEM STATE & SYNCHRONIZATION
// ==========================================
atomic<bool> isInitialized(false);
atomic<bool> isRunning(true);
atomic<bool> schedulerGenerating(false);

uint64_t cpuCycles = 0;
mutex cpuCycleMutex;

deque<shared_ptr<Process>> readyQueue;
vector<shared_ptr<Process>> allProcesses;
mutex processMutex;

// CPU Cores state
vector<shared_ptr<Process>> coreCurrentProcess;
vector<int> coreQuantumRemaining;
mutex coreMutex;

int nextPid = 1;
int processCounter = 1;

// ==========================================
// HELPER UTILITIES
// ==========================================
void clearScreen() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

string getFormattedTimestamp() {
    time_t rawtime;
    struct tm* timeinfo;
    char buffer[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    // Format: (MM/DD/YYYY hh:mm:ssAM/PM)
    strftime(buffer, sizeof(buffer), "(%m/%d/%Y %I:%M:%S%p)", timeinfo);
    return string(buffer);
}

// Clamps uint16 arithmetic
uint16_t clampUint16(int32_t val) {
    if (val < 0) return 0;
    if (val > 65535) return 65535;
    return static_cast<uint16_t>(val);
}

// Generate instruction stream (resolves FOR loop nesting up to 3 levels)
void generateFlatInstructions(vector<Instruction>& flatList, int count, int nestingLevel = 0) {
    static mt19937 rng(1337);
    uniform_int_distribution<int> distType(0, 5);
    uniform_int_distribution<int> distRepeat(2, 4);
    uniform_int_distribution<int> distVal(1, 100);

    for (int i = 0; i < count; i++) {
        int t = distType(rng);
        Instruction ins;

        // Prevent infinite loops or excessive recursion beyond 3 nested loops
        if (t == INS_FOR && nestingLevel >= 3) {
            t = INS_PRINT;
        }

        switch (t) {
            case INS_PRINT:
                ins.type = INS_PRINT;
                break;
            case INS_DECLARE:
                ins.type = INS_DECLARE;
                ins.var1 = "varA";
                ins.val1 = distVal(rng);
                break;
            case INS_ADD:
                ins.type = INS_ADD;
                ins.var1 = "varA";
                ins.var2 = "varA";
                ins.val2 = distVal(rng);
                ins.var2IsVal = false;
                ins.var3IsVal = true;
                break;
            case INS_SUBTRACT:
                ins.type = INS_SUBTRACT;
                ins.var1 = "varA";
                ins.var2 = "varA";
                ins.val2 = distVal(rng);
                ins.var2IsVal = false;
                ins.var3IsVal = true;
                break;
            case INS_SLEEP:
                ins.type = INS_SLEEP;
                ins.sleepTicks = (distVal(rng) % 5) + 1;
                break;
            case INS_FOR: {
                ins.type = INS_FOR;
                int loopReps = distRepeat(rng);
                int subCount = 2;
                vector<Instruction> inner;
                generateFlatInstructions(inner, subCount, nestingLevel + 1);
                // Unroll loop into flat stream
                for (int r = 0; r < loopReps; r++) {
                    flatList.insert(flatList.end(), inner.begin(), inner.end());
                }
                continue;
            }
        }
        flatList.push_back(ins);
    }
}

// Helper to create and register a new process
shared_ptr<Process> createProcess(string name) {
    lock_guard<mutex> lock(processMutex);
    auto p = make_shared<Process>(nextPid++, name);
    p->startTime = getFormattedTimestamp();

    // Determine random line count within config range
    int lines = config.minIns;
    if (config.maxIns > config.minIns) {
        lines = config.minIns + (rand() % (config.maxIns - config.minIns + 1));
    }

    generateFlatInstructions(p->flatInstructions, lines);
    p->totalInstructions = (int)p->flatInstructions.size();

    allProcesses.push_back(p);
    readyQueue.push_back(p);
    return p;
}

// ==========================================
// CPU CORE EXECUTION ENGINE
// ==========================================
void executeInstruction(shared_ptr<Process>& p, Instruction& ins, int coreId) {
    switch (ins.type) {
        case INS_PRINT: {
            stringstream ss;
            ss << getFormattedTimestamp() << " Core:" << coreId 
               << " \"Hello world from " << p->name << "!\"";
            p->logs.push_back(ss.str());
            break;
        }
        case INS_DECLARE: {
            p->memory[ins.var1] = clampUint16(ins.val1);
            break;
        }
        case INS_ADD: {
            uint16_t v2 = ins.var2IsVal ? ins.val1 : p->memory[ins.var2];
            uint16_t v3 = ins.var3IsVal ? ins.val2 : p->memory[ins.var3];
            p->memory[ins.var1] = clampUint16((int32_t)v2 + (int32_t)v3);
            break;
        }
        case INS_SUBTRACT: {
            uint16_t v2 = ins.var2IsVal ? ins.val1 : p->memory[ins.var2];
            uint16_t v3 = ins.var3IsVal ? ins.val2 : p->memory[ins.var3];
            p->memory[ins.var1] = clampUint16((int32_t)v2 - (int32_t)v3);
            break;
        }
        case INS_SLEEP: {
            p->sleepRemaining = ins.sleepTicks;
            p->state = WAITING_SLEEP;
            break;
        }
        default:
            break;
    }
}

// Thread function for individual CPU worker
void cpuWorkerThread(int coreId) {
    while (isRunning) {
        shared_ptr<Process> p = nullptr;

        // Fetch task from Ready Queue
        {
            lock_guard<mutex> lock(processMutex);
            if (!readyQueue.empty()) {
                p = readyQueue.front();
                readyQueue.pop_front();
                p->state = RUNNING;
                p->assignedCore = coreId;
            }
        }

        {
            lock_guard<mutex> cLock(coreMutex);
            coreCurrentProcess[coreId] = p;
            coreQuantumRemaining[coreId] = config.quantumCycles;
        }

        if (p == nullptr) {
            this_thread::sleep_for(chrono::milliseconds(5));
            continue;
        }

        // Process Execution Loop
        while (isRunning && p->state == RUNNING) {
            // Check delays-per-exec (busy-waiting)
            if (config.delaysPerExec > 0) {
                this_thread::sleep_for(chrono::milliseconds(config.delaysPerExec));
            }

            // Execute current instruction
            if (p->currentInstructionIndex < p->totalInstructions) {
                Instruction& ins = p->flatInstructions[p->currentInstructionIndex];
                executeInstruction(p, ins, coreId);
                p->currentInstructionIndex++;
            }

            // Check if process finished
            if (p->currentInstructionIndex >= p->totalInstructions) {
                p->state = FINISHED;
                break;
            }

            // Process requested SLEEP
            if (p->state == WAITING_SLEEP) {
                break;
            }

            // Round Robin quantum slice decrement
            if (config.scheduler == "rr") {
                lock_guard<mutex> cLock(coreMutex);
                coreQuantumRemaining[coreId]--;
                if (coreQuantumRemaining[coreId] <= 0) {
                    p->state = READY;
                    lock_guard<mutex> lock(processMutex);
                    readyQueue.push_back(p);
                    break;
                }
            }

            // Simulate baseline execution tick duration
            this_thread::sleep_for(chrono::milliseconds(2));
        }

        // Relinquish core
        {
            lock_guard<mutex> cLock(coreMutex);
            coreCurrentProcess[coreId] = nullptr;
        }
    }
}

// Master ticker thread managing CPU cycles, batch generator, and sleeping processes
void systemTickerThread() {
    uint32_t batchCounter = 0;

    while (isRunning) {
        this_thread::sleep_for(chrono::milliseconds(10));

        if (!isInitialized) continue;

        {
            lock_guard<mutex> lock(cpuCycleMutex);
            cpuCycles++;
        }

        // 1. Manage Sleeping processes
        {
            lock_guard<mutex> lock(processMutex);
            for (auto& p : allProcesses) {
                if (p->state == WAITING_SLEEP) {
                    if (p->sleepRemaining > 0) {
                        p->sleepRemaining--;
                    }
                    if (p->sleepRemaining == 0) {
                        p->state = READY;
                        readyQueue.push_back(p);
                    }
                }
            }
        }

        // 2. Generate batch processes if enabled
        if (schedulerGenerating) {
            batchCounter++;
            if (batchCounter >= config.batchProcessFreq) {
                batchCounter = 0;
                stringstream nameStream;
                nameStream << "p" << setfill('0') << setw(2) << processCounter++;
                createProcess(nameStream.str());
            }
        }
    }
}

// ==========================================
// CONFIGURATION LOADER
// ==========================================
bool loadConfig(const string& filename) {
    ifstream file(filename);
    if (!file.is_open()) return false;

    string key;
    while (file >> key) {
        if (key == "num-cpu") {
            file >> config.numCpu;
            if (config.numCpu < 1) config.numCpu = 1;
            if (config.numCpu > 128) config.numCpu = 128;
        } else if (key == "scheduler") {
            string s;
            file >> s;
            // Strip outer quotes if present
            s.erase(remove(s.begin(), s.end(), '\"'), s.end());
            config.scheduler = s;
        } else if (key == "quantum-cycles") {
            file >> config.quantumCycles;
        } else if (key == "batch-process-freq") {
            file >> config.batchProcessFreq;
        } else if (key == "min-ins") {
            file >> config.minIns;
        } else if (key == "max-ins") {
            file >> config.maxIns;
        } else if (key == "delays-per-exec") {
            file >> config.delaysPerExec;
        }
    }
    return true;
}

// ==========================================
// SCREEN -LS / REPORT GENERATION HELPER
// ==========================================
string buildUtilizationReport() {
    stringstream out;
    int usedCores = 0;

    {
        lock_guard<mutex> lock(coreMutex);
        for (int i = 0; i < config.numCpu; i++) {
            if (coreCurrentProcess[i] != nullptr) {
                usedCores++;
            }
        }
    }

    int availableCores = config.numCpu - usedCores;
    double utilization = (config.numCpu > 0) ? ((double)usedCores / config.numCpu) * 100.0 : 0.0;

    out << "CPU utilization: " << (int)utilization << "%\n";
    out << "Cores used: " << usedCores << "\n";
    out << "Cores available: " << availableCores << "\n\n";

    out << "Running processes:\n";
    {
        lock_guard<mutex> lock(processMutex);
        for (const auto& p : allProcesses) {
            if (p->state == RUNNING || p->state == READY || p->state == WAITING_SLEEP) {
                out << left << setw(12) << p->name
                    << " " << p->startTime
                    << " Core: " << (p->assignedCore != -1 ? to_string(p->assignedCore) : "None")
                    << "\t" << p->currentInstructionIndex << " / " << p->totalInstructions << "\n";
            }
        }

        out << "\nFinished processes:\n";
        for (const auto& p : allProcesses) {
            if (p->state == FINISHED) {
                out << left << setw(12) << p->name
                    << " " << p->startTime
                    << " Finished\t" << p->totalInstructions << " / " << p->totalInstructions << "\n";
            }
        }
    }

    return out.str();
}

// ==========================================
// SCREEN MULTIPLEXER (process-smi, exit)
// ==========================================
void openProcessScreen(shared_ptr<Process> p) {
    clearScreen();
    string input;

    while (true) {
        cout << "root:\\> ";
        if (!getline(cin, input)) break;

        // Trim leading and trailing whitespace
        size_t first = input.find_first_not_of(" \t");
        if (first == string::npos) continue;
        size_t last = input.find_last_not_of(" \t");
        input = input.substr(first, (last - first + 1));

        if (input == "process-smi") {
            cout << "Process name: " << p->name << "\n";
            cout << "ID: " << p->pid << "\n\n";
            cout << "Logs:\n";

            // Print logs
            for (const auto& log : p->logs) {
                cout << log << "\n";
            }

            if (p->state == FINISHED) {
                cout << "\nFinished!\n\n";
            } else {
                cout << "\nCurrent instruction line: " << p->currentInstructionIndex << "\n";
                cout << "Lines of code: " << p->totalInstructions << "\n\n";
            }
        } else if (input == "exit") {
            clearScreen();
            break;
        } else {
            cout << "Unknown command inside screen: " << input << "\n";
        }
    }
}

// ==========================================
// MAIN MENU COMMANDS
// ==========================================
void printHeader() {
    cout << "  ____ ____   ___  ____  _____ ______   __\n";
    cout << " / ___/ ___| / _ \\|  _ \\| ____/ ___\\ \\ / /\n";
    cout << "| |   \\___ \\| | | | |_) |  _| \\___ \\\\ V / \n";
    cout << "| |___ ___) | |_| |  __/| |___ ___) || |  \n";
    cout << " \\____|____/ \\___/|_|   |_____|____/ |_|  \n\n";
    cout << "Welcome to CSOPESY Emulator!\n\n";
    cout << "Developers:\nRance Laus, Aaron Romero, Ezekiel Tiangson, Luis Vito\n\n";
    cout << "Last updated: Nov. 05, 2026\n\n";
    cout << "Type 'initialize' to load config and start the system.\n";
    cout << "Type 'exit' to terminate.\n";
}

void cmdInitialize() {
    if (isInitialized) {
        cout << "System is already initialized.\n";
        return;
    }

    if (!loadConfig("config.txt")) {
        cout << "Warning: Could not open config.txt! Using default settings.\n";
    }

    // Initialize core tracking
    coreCurrentProcess.assign(config.numCpu, nullptr);
    coreQuantumRemaining.assign(config.numCpu, config.quantumCycles);

    // Launch worker threads for each core
    for (int i = 0; i < config.numCpu; i++) {
        thread(cpuWorkerThread, i).detach();
    }

    isInitialized = true;
    cout << "System initialized successfully with " << config.numCpu 
         << " CPU cores. Scheduler: " << config.scheduler << ".\n";
}

void cmdScreen(const string& fullCommand) {
    stringstream ss(fullCommand);
    string cmd, flag, name;
    ss >> cmd >> flag >> name;

    if (flag == "-ls") {
        cout << buildUtilizationReport();
    } else if (flag == "-s") {
        if (name.empty()) {
            cout << "Usage: screen -s <process_name>\n";
            return;
        }

        // Check if exists
        shared_ptr<Process> target = nullptr;
        {
            lock_guard<mutex> lock(processMutex);
            for (auto& p : allProcesses) {
                if (p->name == name) {
                    target = p;
                    break;
                }
            }
        }

        if (target != nullptr) {
            cout << "Process " << name << " already exists!\n";
            return;
        }

        target = createProcess(name);
        openProcessScreen(target);
    } else if (flag == "-r") {
        if (name.empty()) {
            cout << "Usage: screen -r <process_name>\n";
            return;
        }

        shared_ptr<Process> target = nullptr;
        {
            lock_guard<mutex> lock(processMutex);
            for (auto& p : allProcesses) {
                if (p->name == name) {
                    target = p;
                    break;
                }
            }
        }

        if (target == nullptr || target->state == FINISHED) {
            cout << "Process " << name << " not found.\n";
            return;
        }

        openProcessScreen(target);
    } else {
        cout << "Unknown screen option. Usage: screen -s <name> | screen -r <name> | screen -ls\n";
    }
}

void cmdReportUtil() {
    string report = buildUtilizationReport();
    ofstream outFile("csopesy-log.txt");
    if (outFile.is_open()) {
        outFile << report;
        outFile.close();
        cout << "Report generated at csopesy-log.txt\n";
    } else {
        cout << "Error: Could not write to csopesy-log.txt\n";
    }
}

// ==========================================
// MAIN FUNCTION
// ==========================================
int main() {
    srand((unsigned int)time(NULL));

    // Launch ticker thread
    thread(systemTickerThread).detach();

    printHeader();

    string line;
    while (isRunning) {
        cout << "\nroot:\\> ";
        if (!getline(cin, line)) break;

        size_t first = line.find_first_not_of(" \t");
        if (first == string::npos) continue;
        size_t last = line.find_last_not_of(" \t");
        string trimmed = line.substr(first, (last - first + 1));

        stringstream ss(trimmed);
        string rootCommand;
        ss >> rootCommand;

        // Global commands
        if (rootCommand == "exit") {
            isRunning = false;
            break;
        }

        if (rootCommand == "clear") {
            clearScreen();
            printHeader();
            continue;
        }

        // Before 'initialize', no other commands are recognized
        if (!isInitialized) {
            if (rootCommand == "initialize") {
                cmdInitialize();
            } else {
                cout << "Command not recognized. Please run 'initialize' first.\n";
            }
            continue;
        }

        // Recognized commands post-initialization
        if (rootCommand == "initialize") {
            cmdInitialize();
        } else if (rootCommand == "screen") {
            cmdScreen(trimmed);
        } else if (rootCommand == "scheduler-start") {
            schedulerGenerating = true;
            cout << "Dummy process generation started.\n";
        } else if (rootCommand == "scheduler-stop") {
            schedulerGenerating = false;
            cout << "Dummy process generation stopped.\n";
        } else if (rootCommand == "report-util") {
            cmdReportUtil();
        } else {
            cout << "Unknown command: " << rootCommand << "\n";
        }
    }

    return 0;
}