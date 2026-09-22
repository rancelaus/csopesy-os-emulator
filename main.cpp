#include <iostream>
#include <string>
using namespace std;

// Function prototypes

void printHeader() {
    // TODO: ASCII text header “CSOPESY" + other header texts
}

void cmdInitialize() {
    cout << "initialize command recognized. doing something..." << endl;
}

void cmdScreen() {
    cout << "screen command recognized. doing something..." << endl;
}

void cmdSchedulerStart() {
    cout << "scheduler-start command recognized. doing something..." << endl;
}

void cmdSchedulerStop() {
    cout << "scheduler-stop command recognized. doing something..." << endl;
}

void cmdReportUtil() {
    cout << "report-util command recognized. doing something..." << endl;
}

void cmdClear() {
    // TODO: the screen clears, reprinting the header texts
}

void cmdExit() {
    // TODO: the application/CLI immediately closes
}

// Main program

int main() {
    bool isRunning = true;
    string command;

    // Displays initial header
    printHeader();

    // Main loop for command inputs
    while (isRunning) {
        cout << endl << "Enter a command: "; // Prompt for command
        getline(cin, command); // Read user input

        // Process commands
        if (command == "initialize") {
            cmdInitialize();
        } else if (command == "screen") {
            cmdScreen();
        } else if (command == "scheduler-start") {
            cmdSchedulerStart();
        } else if (command == "scheduler-stop") {
            cmdSchedulerStop();
        } else if (command == "report-util") {
            cmdReportUtil();
        } else if (command == "clear") {
            cmdClear();
        } else if (command == "exit") {
            isRunning = false; // Exit the loop
            cmdExit();
        } else {
            cout << "Unknown command: " << command << endl;
        }
    }

    return 0;
}