#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <ctime>
#include <iomanip>
#include <algorithm>
#include <cstdlib>

using namespace std;

// Holds all the settings read from config.txt
struct Config {
    int numCpu = 4;
    string scheduler = "rr"; // can be "fcfs" or "rr"
    uint32_t quantumCycles = 5;
    uint32_t batchProcessFreq = 1;
    uint32_t minIns = 1000;
    uint32_t maxIns = 2000;
    uint32_t delaysPerExec = 0;
} config;

// The types of instructions processes can execute
enum InstructionType {
    INS_PRINT,
    INS_DECLARE,
    INS_ADD,
    INS_SUBTRACT,
    INS_SLEEP
};

// Represents an individual instruction line
struct Instruction {
    InstructionType type;
    uint8_t sleepTicks = 0; // used only if type is INS_SLEEP
};

// Possible states a process can be in
enum ProcessState {
    READY,
    RUNNING,
    WAITING_SLEEP,
    FINISHED
};

// Process Control Block (PCB) to track process details
struct Process {
    int pid;
    string name;
    ProcessState state;
    vector<Instruction> instructions;
    int totalInstructions = 0;
    int currentLine = 0;
    int assignedCore = -1;
    string startTime;

    // Simulated 16-bit registers/variables clamped to [0, 65535]
    uint16_t varA = 0;
    vector<string> logs;
    uint8_t sleepRemaining = 0;

    Process(int id, string pName) : pid(id), name(pName), state(READY), currentLine(0) {}
};

// Global flags and synchronization
atomic<bool> isInitialized(false);
atomic<bool> isRunning(true);
atomic<bool> schedulerGenerating(false);

deque<shared_ptr<Process>> readyQueue;
vector<shared_ptr<Process>> allProcesses;
mutex processMutex; // protects readyQueue and allProcesses

// Keeps track of what each core is doing
vector<shared_ptr<Process>> coreCurrentProcess;
vector<int> coreQuantumRemaining;
mutex coreMutex;

int nextPid = 1;
int processCounter = 1;

// Helper to clear the terminal window
void clearScreen() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

// Generates timestamp in format: (MM/DD/YYYY hh:mm:ssAM/PM)
string getCurrentTimestamp() {
    time_t rawtime;
    struct tm* timeinfo;
    char buffer[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buffer, sizeof(buffer), "(%m/%d/%Y %I:%M:%S%p)", timeinfo);
    return string(buffer);
}

// Generates instructions for a process
// Flattens loops directly into the instruction array (loops can nest up to 3 times)
void generateInstructions(vector<Instruction>& list, int targetCount, int depth = 0) {
    while ((int)list.size() < targetCount) {
        int roll = rand() % 6; // 0: print, 1: declare, 2: add, 3: sub, 4: sleep, 5: for-loop

        if (roll == 5 && depth < 3) {
            // Simulate a simple FOR loop by repeating a small block of instructions
            int repeats = 2 + (rand() % 3);
            vector<Instruction> loopBlock;
            generateInstructions(loopBlock, 3, depth + 1);

            for (int r = 0; r < repeats && (int)list.size() < targetCount; r++) {
                list.insert(list.end(), loopBlock.begin(), loopBlock.end());
            }
        } else {
            Instruction ins;
            if (roll == 0 || roll == 5) ins.type = INS_PRINT;
            else if (roll == 1) ins.type = INS_DECLARE;
            else if (roll == 2) ins.type = INS_ADD;
            else if (roll == 3) ins.type = INS_SUBTRACT;
            else if (roll == 4) {
                ins.type = INS_SLEEP;
                ins.sleepTicks = 1 + (rand() % 5);
            }
            list.push_back(ins);
        }
    }
}

// Creates and registers a new process, then pushes it to the ready queue
shared_ptr<Process> createProcess(string name) {
    lock_guard<mutex> lock(processMutex);

    auto p = make_shared<Process>(nextPid++, name);
    p->startTime = getCurrentTimestamp();

    // Pick random instruction count based on config settings
    int lineRange = config.maxIns - config.minIns + 1;
    int lines = (lineRange > 0) ? config.minIns + (rand() % lineRange) : config.minIns;

    generateInstructions(p->instructions, lines);
    p->totalInstructions = (int)p->instructions.size();

    allProcesses.push_back(p);
    readyQueue.push_back(p);
    return p;
}

// Executes one instruction for a process on a specific core
void runInstruction(shared_ptr<Process>& p, Instruction& ins, int coreId) {
    switch (ins.type) {
        case INS_PRINT: {
            // Required print message format from specs
            string logEntry = getCurrentTimestamp() + " Core:" + to_string(coreId) + 
                              " \"Hello world from " + p->name + "!\"";
            p->logs.push_back(logEntry);
            break;
        }
        case INS_DECLARE:
            p->varA = 10;
            break;
        case INS_ADD:
            // uint16 clamping [0, 65535]
            if ((int)p->varA + 5 > 65535) p->varA = 65535;
            else p->varA += 5;
            break;
        case INS_SUBTRACT:
            if ((int)p->varA - 2 < 0) p->varA = 0;
            else p->varA -= 2;
            break;
        case INS_SLEEP:
            p->sleepRemaining = ins.sleepTicks;
            p->state = WAITING_SLEEP;
            break;
    }
}

// Worker thread for each simulated CPU core
void cpuCoreWorker(int coreId) {
    while (isRunning) {
        shared_ptr<Process> p = nullptr;

        // Try getting a process from the ready queue
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

        // Nothing to run right now, wait a bit
        if (p == nullptr) {
            this_thread::sleep_for(chrono::milliseconds(5));
            continue;
        }

        // Keep executing while process is on this core
        while (isRunning && p->state == RUNNING) {
            // Check for busy-waiting delay
            if (config.delaysPerExec > 0) {
                this_thread::sleep_for(chrono::milliseconds(config.delaysPerExec));
            }

            // Execute the current line
            if (p->currentLine < p->totalInstructions) {
                runInstruction(p, p->instructions[p->currentLine], coreId);
                p->currentLine++;
            }

            // Check if process finished all lines
            if (p->currentLine >= p->totalInstructions) {
                p->state = FINISHED;
                break;
            }

            // If it put itself to sleep, let go of core
            if (p->state == WAITING_SLEEP) {
                break;
            }

            // Handle Round-Robin time slicing
            if (config.scheduler == "rr") {
                lock_guard<mutex> cLock(coreMutex);
                coreQuantumRemaining[coreId]--;

                if (coreQuantumRemaining[coreId] <= 0) {
                    // Quantum expired, move back to ready queue
                    p->state = READY;
                    lock_guard<mutex> lock(processMutex);
                    readyQueue.push_back(p);
                    break;
                }
            }

            this_thread::sleep_for(chrono::milliseconds(1));
        }

        // Release the core
        {
            lock_guard<mutex> cLock(coreMutex);
            coreCurrentProcess[coreId] = nullptr;
        }
    }
}

// Background thread that ticks clock, wakes up sleepers, and generates batch processes
void clockTicker() {
    uint32_t batchTimer = 0;

    while (isRunning) {
        this_thread::sleep_for(chrono::milliseconds(10));
        if (!isInitialized) continue;

        // Wake up sleeping processes
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

        // Automatically spawn processes if scheduler-start is active
        if (schedulerGenerating) {
            batchTimer++;
            if (batchTimer >= config.batchProcessFreq) {
                batchTimer = 0;
                stringstream ss;
                ss << "p" << setfill('0') << setw(2) << processCounter++;
                createProcess(ss.str());
            }
        }
    }
}

// Reads config.txt settings
void loadConfig(const string& filename) {
    ifstream file(filename);
    if (!file.is_open()) {
        cout << "Notice: config.txt not found, applying default settings.\n";
        return;
    }

    string key;
    while (file >> key) {
        if (key == "num-cpu") file >> config.numCpu;
        else if (key == "scheduler") {
            file >> config.scheduler;
            config.scheduler.erase(remove(config.scheduler.begin(), config.scheduler.end(), '\"'), config.scheduler.end());
        }
        else if (key == "quantum-cycles") file >> config.quantumCycles;
        else if (key == "batch-process-freq") file >> config.batchProcessFreq;
        else if (key == "min-ins") file >> config.minIns;
        else if (key == "max-ins") file >> config.maxIns;
        else if (key == "delays-per-exec") file >> config.delaysPerExec;
    }
}

// Builds the summary report used by both "screen -ls" and "report-util"
string buildReport() {
    stringstream out;
    int usedCores = 0;

    {
        lock_guard<mutex> lock(coreMutex);
        for (int i = 0; i < config.numCpu; i++) {
            if (coreCurrentProcess[i] != nullptr) usedCores++;
        }
    }

    int freeCores = config.numCpu - usedCores;
    double utilRate = (config.numCpu > 0) ? ((double)usedCores / config.numCpu) * 100.0 : 0.0;

    out << "CPU utilization: " << (int)utilRate << "%\n";
    out << "Cores used: " << usedCores << "\n";
    out << "Cores available: " << freeCores << "\n\n";

    out << "Running processes:\n";
    {
        lock_guard<mutex> lock(processMutex);
        for (const auto& p : allProcesses) {
            if (p->state != FINISHED) {
                out << left << setw(12) << p->name
                    << " " << p->startTime
                    << " Core: " << (p->assignedCore != -1 ? to_string(p->assignedCore) : "None")
                    << "\t" << p->currentLine << " / " << p->totalInstructions << "\n";
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

// Process screen submenu (handles process-smi and exit)
void openProcessScreen(shared_ptr<Process> p) {
    clearScreen();
    string cmd;

    while (true) {
        cout << "root:\\> ";
        if (!getline(cin, cmd)) break;

        // Trim leading and trailing spaces
        size_t start = cmd.find_first_not_of(" \t");
        if (start == string::npos) continue;
        size_t end = cmd.find_last_not_of(" \t");
        cmd = cmd.substr(start, end - start + 1);

        if (cmd == "process-smi") {
            cout << "Process name: " << p->name << "\n";
            cout << "ID: " << p->pid << "\n\n";
            cout << "Logs:\n";

            for (const auto& log : p->logs) {
                cout << log << "\n";
            }

            if (p->state == FINISHED) {
                cout << "\nFinished!\n\n";
            } else {
                cout << "\nCurrent instruction line: " << p->currentLine << "\n";
                cout << "Lines of code: " << p->totalInstructions << "\n\n";
            }
        } else if (cmd == "exit") {
            clearScreen();
            break;
        } else {
            cout << "Unknown command. Type 'process-smi' or 'exit'.\n";
        }
    }
}

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

    loadConfig("config.txt");

    // Allocate core tracking slots
    coreCurrentProcess.assign(config.numCpu, nullptr);
    coreQuantumRemaining.assign(config.numCpu, config.quantumCycles);

    // Spin up thread workers for the cores
    for (int i = 0; i < config.numCpu; i++) {
        thread(cpuCoreWorker, i).detach();
    }

    isInitialized = true;
    cout << "System initialized successfully with " << config.numCpu 
         << " CPU cores. Scheduler: " << config.scheduler << ".\n";
}

// Parses and routes "screen" subcommands (-s, -r, -ls)
void cmdScreen(const string& fullCommand) {
    stringstream ss(fullCommand);
    string root, flag, name;
    ss >> root >> flag >> name;

    if (flag == "-ls") {
        cout << buildReport();
    } else if (flag == "-s") {
        if (name.empty()) {
            cout << "Usage: screen -s <process_name>\n";
            return;
        }

        // Check if name already in use
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

        // Cannot re-enter a finished or missing process
        if (target == nullptr || target->state == FINISHED) {
            cout << "Process " << name << " not found.\n";
            return;
        }

        openProcessScreen(target);
    } else {
        cout << "Usage: screen -s <name> | screen -r <name> | screen -ls\n";
    }
}

void cmdReportUtil() {
    ofstream file("csopesy-log.txt");
    if (file.is_open()) {
        file << buildReport();
        file.close();
        cout << "Report generated at csopesy-log.txt\n";
    } else {
        cout << "Failed to write report file.\n";
    }
}

int main() {
    srand((unsigned int)time(NULL));

    // Start background clock thread
    thread(clockTicker).detach();

    printHeader();

    string line;
    while (isRunning) {
        cout << "\nroot:\\> ";
        if (!getline(cin, line)) break;

        // Trim leading and trailing whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == string::npos) continue;
        size_t end = line.find_last_not_of(" \t");
        string input = line.substr(start, end - start + 1);

        stringstream ss(input);
        string command;
        ss >> command;

        // Commands allowed prior to initialization
        if (command == "exit") {
            isRunning = false;
            break;
        }
        if (command == "clear") {
            clearScreen();
            printHeader();
            continue;
        }

        // Block everything else until initialized
        if (!isInitialized) {
            if (command == "initialize") {
                cmdInitialize();
            } else {
                cout << "Command not recognized. Please run 'initialize' first.\n";
            }
            continue;
        }

        // Commands recognized after initialize
        if (command == "initialize") {
            cmdInitialize();
        } else if (command == "screen") {
            cmdScreen(input);
        } else if (command == "scheduler-start") {
            schedulerGenerating = true;
            cout << "Dummy process generation started.\n";
        } else if (command == "scheduler-stop") {
            schedulerGenerating = false;
            cout << "Dummy process generation stopped.\n";
        } else if (command == "report-util") {
            cmdReportUtil();
        } else {
            cout << "Unknown command: " << command << "\n";
        }
    }

    return 0;
}