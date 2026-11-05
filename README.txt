CSOPESY Major Output: Process Scheduler and CLI

Developers:
- Rance Laus
- Aaron Romero
- Ezekiel Tiangson
- Luis Vito

Main Entry File: main.cpp

~HOW TO COMPILE AND RUN~

Method A: Using the batch script (Windows)
1. Make sure "config.txt" is placed in the same folder as "main.cpp".
2. Double-click or run "build and run.bat".

Method B: Manual Terminal / Command Prompt
1. Open Command Prompt or Terminal in the project directory.
2. Compile the source code using:
	g++ -std=c++11 -pthread main.cpp -o csopesy_cli.exe
3. Run the executable:
	csopesy_cli.exe

~CONFIGURATION FILE (config.txt)~
The program reads its initial scheduler parameters from "config.txt".
Default parameters:
  num-cpu 4
  scheduler "rr"
  quantum-cycles 5
  batch-process-freq 1
  min-ins 1000
  max-ins 2000
  delays-per-exec 0

~COMMAND REFERENCE~

--- Main Menu Commands ---
- initialize       : Loads parameters from config.txt and boots the 
                     scheduler threads. Must be executed first!
- screen -s <name> : Creates and attaches to a new process screen.
- screen -r <name> : Re-attaches to a currently running process screen.
                     (Note: Finished processes cannot be re-attached).
- screen -ls       : Displays CPU utilization, core statuses, active 
                     processes, and completed processes.
- scheduler-start  : Starts background generation of dummy processes 
                     (p01, p02, etc.).
- scheduler-stop   : Stops background dummy process generation.
- report-util      : Exports CPU utilization and process summaries to 
                     "csopesy-log.txt".
- clear            : Clears the terminal screen.
- exit             : Terminates the emulator program.

--- Inside a Process Screen ---
- process-smi      : Displays process details, logs, instruction 
                     progress, or "Finished!" status.
- exit             : Detaches from the process and returns to the main menu.