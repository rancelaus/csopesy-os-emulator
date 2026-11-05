#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std;

// Holds all settings read from config.txt
struct Config {
  int numCpu = 4;
  string scheduler = "rr"; // "fcfs" or "rr"
  uint32_t quantumCycles = 5;
  uint32_t batchProcessFreq = 1;
  uint32_t minIns = 1000;
  uint32_t maxIns = 2000;
  uint32_t delaysPerExec = 0;
} config;

// Supported process instruction types
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
  uint8_t sleepTicks = 0;  // used if type is INS_SLEEP
  string var1, var2, var3; // variable names for DECLARE/ADD/SUBTRACT/PRINT
  uint16_t val1 = 0, val2 = 0, val3 = 0; // immediate values
  bool isVal2Immediate = false;
  bool isVal3Immediate = false;
  bool printVariable = false; // true if PRINT outputs a variable
};

// Possible states for a process
enum ProcessState { READY, RUNNING, WAITING_SLEEP, FINISHED };

// Process Control Block (PCB)
struct Process {
  int pid;
  string name;
  ProcessState state;
  vector<Instruction> instructions;
  int totalInstructions = 0;
  int currentLine = 0;
  int assignedCore = -1;
  string startTime;

  // Per-process variable store clamped to uint16 [0, 65535]
  map<string, uint16_t> variables;
  vector<string> logs;
  uint64_t wakeTick = 0;
  uint64_t executionDelayRemaining = 0;

  Process(int id, string pName)
      : pid(id), name(pName), state(READY), currentLine(0) {}
};

// Concurrency primitives and state tracking
atomic<bool> isInitialized(false);
atomic<bool> isRunning(true);
bool schedulerGenerating = false;
mutex schedulerMutex;
condition_variable tickReady;
condition_variable tickComplete;
condition_variable clockWake;

deque<shared_ptr<Process>> readyQueue;
vector<shared_ptr<Process>> allProcesses;
vector<shared_ptr<Process>> coreCurrentProcess;
vector<uint64_t> coreQuantumRemaining;
vector<thread> coreWorkers;
thread clockThread;

uint64_t cpuTicks = 0;
uint64_t batchTimer = 0;
size_t pendingCores = 0;
bool tickInProgress = false;
const chrono::milliseconds CPU_TICK_INTERVAL(1); // Wall-clock pacing

int nextPid = 1;
int processCounter = 1;

// Forward declaration
void dispatchCoresLocked();

// Clears the terminal screen
void clearScreen() {
#ifdef _WIN32
  system("cls");
#else
  system("clear");
#endif
}

// Timestamp format: (MM/DD/YYYY hh:mm:ssAM/PM)
string getCurrentTimestamp() {
  time_t rawtime;
  char buffer[80];
  time(&rawtime);

  static mutex timestampMutex;
  lock_guard<mutex> lock(timestampMutex);
  struct tm localTime = *localtime(&rawtime);
  strftime(buffer, sizeof(buffer), "(%m/%d/%Y %I:%M:%S%p)", &localTime);
  return string(buffer);
}

// Helper: Generates variable names from a pool
static string randomVarName() {
  static const char *pool[] = {"x", "y", "z", "a", "b",
                               "c", "d", "e", "f", "g"};
  return pool[rand() % 10];
}

// Generates instructions; flattens FOR loops up to 3 nesting levels
void generateInstructions(vector<Instruction> &list, int targetCount,
                          int depth = 0) {
  while ((int)list.size() < targetCount) {
    int roll = rand() % 6; // 0: print, 1: declare, 2: add, 3: sub, 4: sleep, 5: for-loop

    if (roll == 5 && depth < 3) {
      int repeats = 2 + (rand() % 3);
      vector<Instruction> loopBlock;
      generateInstructions(loopBlock, 3, depth + 1);

      for (int r = 0; r < repeats && (int)list.size() < targetCount; r++) {
        list.insert(list.end(), loopBlock.begin(), loopBlock.end());
      }
    } else {
      Instruction ins;
      if (roll == 0 || roll == 5) {
        ins.type = INS_PRINT;
        if (rand() % 3 == 0) {
          ins.printVariable = true;
          ins.var1 = randomVarName();
        }
      } else if (roll == 1) {
        ins.type = INS_DECLARE;
        ins.var1 = randomVarName();
        ins.val1 = (uint16_t)(rand() % 100);
      } else if (roll == 2) {
        ins.type = INS_ADD;
        ins.var1 = randomVarName();
        ins.var2 = randomVarName();
        if (rand() % 2 == 0) {
          ins.var3 = randomVarName();
        } else {
          ins.isVal3Immediate = true;
          ins.val3 = (uint16_t)(rand() % 50);
        }
      } else if (roll == 3) {
        ins.type = INS_SUBTRACT;
        ins.var1 = randomVarName();
        ins.var2 = randomVarName();
        if (rand() % 2 == 0) {
          ins.var3 = randomVarName();
        } else {
          ins.isVal3Immediate = true;
          ins.val3 = (uint16_t)(rand() % 50);
        }
      } else if (roll == 4) {
        ins.type = INS_SLEEP;
        ins.sleepTicks = 1 + (rand() % 5);
      }
      list.push_back(ins);
    }
  }
}

// Creates and registers a new process
shared_ptr<Process> createProcessLocked(const string &name) {
  for (const auto &existing : allProcesses) {
    if (existing->name == name)
      return nullptr;
  }

  auto p = make_shared<Process>(nextPid++, name);
  p->startTime = getCurrentTimestamp();
  p->executionDelayRemaining = 0;

  int lineRange = config.maxIns - config.minIns + 1;
  int lines =
      (lineRange > 0) ? config.minIns + (rand() % lineRange) : config.minIns;

  generateInstructions(p->instructions, lines);
  p->totalInstructions = (int)p->instructions.size();

  allProcesses.push_back(p);
  readyQueue.push_back(p);
  dispatchCoresLocked();
  return p;
}

shared_ptr<Process> createProcess(const string &name) {
  lock_guard<mutex> lock(schedulerMutex);
  return createProcessLocked(name);
}

// Executes one instruction for a process on a specific core
void runInstruction(shared_ptr<Process> &p, Instruction &ins, int coreId) {
  switch (ins.type) {
  case INS_PRINT: {
    string msgContent;
    if (ins.printVariable && !ins.var1.empty()) {
      uint16_t val = p->variables[ins.var1];
      msgContent = "\"Value from: " + ins.var1 + " = " + to_string(val) + "\"";
    } else {
      msgContent = "\"Hello world from " + p->name + "!\"";
    }
    string logEntry =
        getCurrentTimestamp() + " Core:" + to_string(coreId) + " " + msgContent;
    p->logs.push_back(logEntry);
    break;
  }
  case INS_DECLARE:
    p->variables[ins.var1] = ins.val1;
    break;
  case INS_ADD: {
    uint32_t op2 = ins.isVal2Immediate ? ins.val2 : p->variables[ins.var2];
    uint32_t op3 = ins.isVal3Immediate ? ins.val3 : p->variables[ins.var3];
    uint32_t sum = op2 + op3;
    p->variables[ins.var1] = (sum > 65535) ? (uint16_t)65535 : (uint16_t)sum;
    break;
  }
  case INS_SUBTRACT: {
    int32_t op2 = ins.isVal2Immediate ? (int32_t)ins.val2
                                      : (int32_t)p->variables[ins.var2];
    int32_t op3 = ins.isVal3Immediate ? (int32_t)ins.val3
                                      : (int32_t)p->variables[ins.var3];
    int32_t diff = op2 - op3;
    p->variables[ins.var1] = (diff < 0) ? (uint16_t)0 : (uint16_t)diff;
    break;
  }
  case INS_SLEEP:
    p->wakeTick = cpuTicks + max<uint64_t>(1, ins.sleepTicks);
    p->state = WAITING_SLEEP;
    break;
  }
}

// Release ownership before making a process visible in the ready queue.
void releaseCoreLocked(int coreId, ProcessState nextState) {
  auto p = coreCurrentProcess[coreId];
  coreCurrentProcess[coreId].reset();
  coreQuantumRemaining[coreId] = 0;
  p->assignedCore = -1;
  p->state = nextState;
  if (nextState == READY)
    readyQueue.push_back(p);
}

void dispatchCoresLocked() {
  for (int coreId = 0; coreId < config.numCpu; ++coreId) {
    if (!coreCurrentProcess[coreId] && !readyQueue.empty()) {
      auto p = readyQueue.front();
      readyQueue.pop_front();
      p->state = RUNNING;
      p->assignedCore = coreId;
      coreCurrentProcess[coreId] = p;
      coreQuantumRemaining[coreId] = config.quantumCycles;
    }
  }
}

// Dispatch all cores before any worker executes, preventing a process from
// migrating to another core and executing twice in the same global tick.
void beginTickLocked() {
  ++cpuTicks;
  for (const auto &p : allProcesses) {
    if (p->state == WAITING_SLEEP && cpuTicks >= p->wakeTick) {
      p->state = READY;
      readyQueue.push_back(p);
    }
  }
  dispatchCoresLocked();
}

// Executes an occupied core cycle
void executeCoreTickLocked(int coreId) {
  auto p = coreCurrentProcess[coreId];
  if (!p)
    return;

  if (p->executionDelayRemaining > 0) {
    --p->executionDelayRemaining;
  } else {
    if (p->currentLine < p->totalInstructions) {
      runInstruction(p, p->instructions[p->currentLine], coreId);
      ++p->currentLine;
    }
    p->executionDelayRemaining = config.delaysPerExec;
  }

  if (p->currentLine >= p->totalInstructions) {
    releaseCoreLocked(coreId, FINISHED);
  } else if (p->state == WAITING_SLEEP) {
    releaseCoreLocked(coreId, WAITING_SLEEP);
  } else if (config.scheduler == "rr" && --coreQuantumRemaining[coreId] == 0) {
    releaseCoreLocked(coreId, READY);
  }
}

// Generation occurs at the END of each configured number of global cycles.
void finishTickLocked() {
  if (schedulerGenerating && ++batchTimer >= config.batchProcessFreq) {
    batchTimer = 0;
    shared_ptr<Process> created;
    do {
      stringstream name;
      name << "p" << setfill('0') << setw(2) << processCounter++;
      created = createProcessLocked(name.str());
    } while (!created);
  }
  dispatchCoresLocked();
}

void cpuCoreWorker(int coreId) {
  uint64_t lastTick = 0;
  unique_lock<mutex> lock(schedulerMutex);
  while (true) {
    tickReady.wait(lock, [&] {
      return !isRunning || (tickInProgress && cpuTicks != lastTick);
    });
    if (!isRunning)
      return;
    lastTick = cpuTicks;
    executeCoreTickLocked(coreId);
    if (--pendingCores == 0)
      tickComplete.notify_one();
  }
}

void clockTicker() {
  unique_lock<mutex> lock(schedulerMutex);
  auto lastGenTime = chrono::steady_clock::now();

  while (isRunning) {
    bool hasWork = !readyQueue.empty();
    for (const auto& cp : coreCurrentProcess) {
      if (cp) { hasWork = true; break; }
    }

    auto sleepDuration = hasWork ? chrono::microseconds(100) : chrono::milliseconds(10);
    if (clockWake.wait_for(lock, sleepDuration, [] { return !isRunning; }))
      break;

    beginTickLocked();
    pendingCores = coreWorkers.size();
    tickInProgress = true;
    tickReady.notify_all();
    tickComplete.wait(lock, [] { return !isRunning || pendingCores == 0; });
    tickInProgress = false;
    if (!isRunning) break;

    // Pace process generation to ~1 batch per second when scheduler-test is on
    auto now = chrono::steady_clock::now();
    if (schedulerGenerating && chrono::duration_cast<chrono::milliseconds>(now - lastGenTime).count() >= 1000) {
      lastGenTime = now;
      finishTickLocked();
    } else {
      dispatchCoresLocked();
    }
  }
}

void setSchedulerGenerating(bool enabled) {
  lock_guard<mutex> lock(schedulerMutex);
  if (enabled && !schedulerGenerating)
    batchTimer = 0;
  schedulerGenerating = enabled;
  if (!enabled)
    batchTimer = 0;
}

void shutdownScheduler() {
  {
    lock_guard<mutex> lock(schedulerMutex);
    isRunning = false;
    schedulerGenerating = false;
  }
  tickReady.notify_all();
  tickComplete.notify_all();
  clockWake.notify_all();
  if (clockThread.joinable())
    clockThread.join();
  for (auto &worker : coreWorkers) {
    if (worker.joinable())
      worker.join();
  }
  coreWorkers.clear();
}

struct ProcessSnapshot {
  int pid;
  string name;
  ProcessState state;
  int currentLine;
  int totalInstructions;
  int assignedCore;
  string startTime;
  vector<string> logs;
};

ProcessSnapshot snapshotProcessLocked(const shared_ptr<Process> &p,
                                      bool includeLogs) {
  ProcessSnapshot result;
  result.pid = p->pid;
  result.name = p->name;
  result.state = p->state;
  result.currentLine = p->currentLine;
  result.totalInstructions = p->totalInstructions;
  result.assignedCore = p->assignedCore;
  result.startTime = p->startTime;
  if (includeLogs)
    result.logs = p->logs;
  return result;
}

ProcessSnapshot getProcessSnapshot(const shared_ptr<Process> &p) {
  lock_guard<mutex> lock(schedulerMutex);
  return snapshotProcessLocked(p, true);
}

struct SchedulerSnapshot {
  uint64_t ticks;
  int totalCores;
  int usedCores;
  vector<ProcessSnapshot> processes;
};

SchedulerSnapshot getSchedulerSnapshot() {
  lock_guard<mutex> lock(schedulerMutex);
  dispatchCoresLocked();
  SchedulerSnapshot result;
  result.ticks = cpuTicks;
  result.totalCores = config.numCpu;
  result.usedCores = 0;
  for (const auto &p : coreCurrentProcess)
    if (p)
      ++result.usedCores;
  for (const auto &p : allProcesses) {
    result.processes.push_back(snapshotProcessLocked(p, false));
  }
  return result;
}

// Reads configuration parameters
void loadConfig(const string &filename) {
  ifstream file(filename);
  if (!file.is_open()) {
    cout << "Notice: config.txt not found, applying default settings.\n";
    return;
  }

  string key;
  while (file >> key) {
    if (key == "num-cpu")
      file >> config.numCpu;
    else if (key == "scheduler") {
      file >> config.scheduler;
      config.scheduler.erase(
          remove(config.scheduler.begin(), config.scheduler.end(), '\"'),
          config.scheduler.end());
    } else if (key == "quantum-cycles")
      file >> config.quantumCycles;
    else if (key == "batch-process-freq")
      file >> config.batchProcessFreq;
    else if (key == "min-ins")
      file >> config.minIns;
    else if (key == "max-ins")
      file >> config.maxIns;
    else if (key == "delays-per-exec" || key == "delay-per-exec")
      file >> config.delaysPerExec;
  }
}

// Builds the summary report used by both "screen -ls" and "report-util"
string buildReport() {
  stringstream out;
  const auto snapshot = getSchedulerSnapshot();
  const int usedCores = snapshot.usedCores;
  const int freeCores = snapshot.totalCores - usedCores;
  const double utilRate =
      snapshot.totalCores > 0 ? 100.0 * usedCores / snapshot.totalCores : 0.0;

  out << "CPU utilization: " << (int)utilRate << "%\n";
  out << "Cores used: " << usedCores << "\n";
  out << "Cores available: " << freeCores << "\n\n";
  out << "--------------------------------------\n";

  // list processes actively running on cores
  out << "Running processes:\n";
  for (const auto &p : snapshot.processes) {
    if (p.state == RUNNING && p.assignedCore != -1) {
      out << left << setw(12) << p.name << " " << p.startTime
          << " Core: " << p.assignedCore << " \t" << p.currentLine << " / "
          << p.totalInstructions << "\n";
    }
  }

  out << "\nFinished processes:\n";
  for (const auto &p : snapshot.processes) {
    if (p.state == FINISHED) {
      out << left << setw(12) << p.name << " " << p.startTime << " Finished\t"
          << p.totalInstructions << " / " << p.totalInstructions << "\n";
    }
  }

  out << "--------------------------------------\n";
  return out.str();
}

// Print process details
void renderProcessSmi(const shared_ptr<Process> &p) {
  const auto snapshot = getProcessSnapshot(p);
  cout << "Process name: " << snapshot.name << "\n";
  cout << "ID: " << snapshot.pid << "\n\n";
  cout << "Logs:\n";

  for (const auto &log : snapshot.logs) {
    cout << log << "\n";
  }

  if (snapshot.state == FINISHED) {
    cout << "\nFinished!\n\n";
  } else {
    cout << "\nCurrent instruction line: " << snapshot.currentLine << "\n";
    cout << "Lines of code: " << snapshot.totalInstructions << "\n\n";
  }
}

// Attached process screen sub-shell
void openProcessScreen(shared_ptr<Process> p) {
  clearScreen();
  // Render process status upon entering the screen
  renderProcessSmi(p);

  string cmd;
  while (true) {
    cout << "root:\\> ";
    if (!getline(cin, cmd))
      break;

    size_t start = cmd.find_first_not_of(" \t");
    if (start == string::npos)
      continue;
    size_t end = cmd.find_last_not_of(" \t");
    cmd = cmd.substr(start, end - start + 1);

    if (cmd == "process-smi") {
      renderProcessSmi(p);
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
  cout << " \\____|____/ \\___/|_|   |_____|____/ |_|  \n";
  cout << "--------------------------------------------------\n";
  cout << "Welcome to CSOPESY Emulator!\n\n";
  cout << "Developers:\nRance Laus, Aaron Romero, Ezekiel Tiangson, Luis "
          "Vito\n\n";
  cout << "Last updated: Nov. 05, 2026\n";
  cout << "--------------------------------------------------\n\n";
  cout << "Type 'initialize' to load config and start the system.\n";
  cout << "Type 'exit' to terminate.\n";
}

void cmdInitialize() {
  if (isInitialized) {
    cout << "System is already initialized.\n";
    return;
  }

  loadConfig("config.txt");

  if (config.numCpu < 1 || config.numCpu > 128 ||
      (config.scheduler != "fcfs" && config.scheduler != "rr") ||
      config.quantumCycles < 1 || config.batchProcessFreq < 1 ||
      config.minIns < 1 || config.maxIns < config.minIns ||
      config.maxIns > static_cast<uint32_t>(numeric_limits<int>::max())) {
    cout << "Invalid scheduler configuration; initialization cancelled.\n";
    return;
  }

  coreCurrentProcess.assign(config.numCpu, nullptr);
  coreQuantumRemaining.assign(config.numCpu, config.quantumCycles);
  try {
    for (int i = 0; i < config.numCpu; ++i) {
      coreWorkers.emplace_back(cpuCoreWorker, i);
    }
    clockThread = thread(clockTicker);
  } catch (const exception &error) {
    shutdownScheduler();
    isRunning = true;
    cout << "Unable to start scheduler: " << error.what() << "\n";
    return;
  }
  isInitialized = true;
  cout << "System initialized successfully with " << config.numCpu
       << " CPU cores. Scheduler: " << config.scheduler << ".\n";
}

void cmdScreen(const string &fullCommand) {
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

    shared_ptr<Process> target = nullptr;
    {
      lock_guard<mutex> lock(schedulerMutex);
      for (auto &p : allProcesses) {
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
    if (!target) {
      cout << "Process " << name << " already exists!\n";
      return;
    }
    openProcessScreen(target);
  } else if (flag == "-r") {
    if (name.empty()) {
      cout << "Usage: screen -r <process_name>\n";
      return;
    }

    shared_ptr<Process> target = nullptr;
    {
      lock_guard<mutex> lock(schedulerMutex);
      for (auto &p : allProcesses) {
        if (p->name == name) {
          target = p;
          break;
        }
      }
    }

    if (target == nullptr || getProcessSnapshot(target).state == FINISHED) {
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
    cout << "Report generated at csopesy-log.txt!\n";
  } else {
    cout << "Failed to write report file.\n";
  }
}

int main() {
  srand((unsigned int)time(NULL));

  printHeader();

  string line;
  while (isRunning) {
    cout << "\nroot:\\> ";
    if (!getline(cin, line))
      break;

    size_t start = line.find_first_not_of(" \t");
    if (start == string::npos)
      continue;
    size_t end = line.find_last_not_of(" \t");
    string input = line.substr(start, end - start + 1);

    stringstream ss(input);
    string command;
    ss >> command;

    if (command == "exit") {
      isRunning = false;
      break;
    }
    if (command == "clear") {
      clearScreen();
      printHeader();
      continue;
    }

    if (!isInitialized) {
      if (command == "initialize") {
        cmdInitialize();
      } else {
        cout << "Command not recognized. Please run 'initialize' first.\n";
      }
      continue;
    }

    if (command == "initialize") {
      cmdInitialize();
    } else if (command == "screen") {
      cmdScreen(input);
    } else if (command == "screen-ls") {
      cout << buildReport();
    } else if (command == "scheduler-start" || command == "scheduler-test") {
      setSchedulerGenerating(true);
      cout << "Dummy process generation started.\n";
    } else if (command == "scheduler-stop") {
      setSchedulerGenerating(false);
      cout << "Dummy process generation stopped.\n";
    } else if (command == "report-util") {
      cmdReportUtil();
    } else {
      cout << "Unknown command: " << command << "\n";
    }
  }

  shutdownScheduler();
  return 0;
}