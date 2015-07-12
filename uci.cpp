#include <iostream>
#include <string>
#include <sstream>
#include <vector>

#include "uci.h"
using namespace std;

// Splits a string s with delimiter d into vector v.
void split(const string &s, char d, vector<string> &v) {
    stringstream ss(s);
    string item;
    while (getline(ss, item, d)) {
        v.push_back(item);
    }
}

// Splits a string s with delimiter d.
vector<string> split(const string &s, char d) {
    vector<string> v;
    split(s, d, v);
    return v;
}

Board fenToBoard(string s) {
    vector<string> components = split(s, ' ');
    vector<string> rows = split(components.at(0), '/');
    int mailbox[64];
    
    int colCounter;
    
    // iterate through rows, converting into mailbox format
    for (int elem = 0; elem < 8; elem++) {
        string rowAtElem = rows.at(elem);
        
        colCounter = 0;
        
        for (unsigned col = 0; col < rowAtElem.length(); col++) {
            // determine what piece is on rowAtElem.at(col) and fill out if not blank
            switch (rowAtElem.at(col)) {
                case 'P':
                    mailbox[8 * (7 - elem) + colCounter] = WHITE + PAWNS;
                    break;
                case 'N':
                    mailbox[8 * (7 - elem) + colCounter] = WHITE + KNIGHTS;
                    break;
                case 'B':
                    mailbox[8 * (7 - elem) + colCounter] = WHITE + BISHOPS;
                    break;
                case 'R':
                    mailbox[8 * (7 - elem) + colCounter] = WHITE + ROOKS;
                    break;
                case 'Q':
                    mailbox[8 * (7 - elem) + colCounter] = WHITE + QUEENS;
                    break;
                case 'K':
                    mailbox[8 * (7 - elem) + colCounter] = WHITE + KINGS;
                    break;
                case 'p':
                    mailbox[8 * (7 - elem) + colCounter] = BLACK + PAWNS;
                    break;
                case 'n':
                    mailbox[8 * (7 - elem) + colCounter] = BLACK + KNIGHTS;
                    break;
                case 'b':
                    mailbox[8 * (7 - elem) + colCounter] = BLACK + BISHOPS;
                    break;
                case 'r':
                    mailbox[8 * (7 - elem) + colCounter] = BLACK + ROOKS;
                    break;
                case 'q':
                    mailbox[8 * (7 - elem) + colCounter] = BLACK + QUEENS;
                    break;
                case 'k':
                    mailbox[8 * (7 - elem) + colCounter] = BLACK + KINGS;
                    break;
            }
            
            if (rowAtElem.at(col) >= 'B') colCounter++;
            
            // fill out blank squares
            if ('1' <= rowAtElem.at(col) && rowAtElem.at(col) <= '8') {
                for (int i = 0; i < rowAtElem.at(col) - '0'; i++) {
                    mailbox[8 * (7 - elem) + colCounter] = -1; // -1 is blank square
                    colCounter++;
                }
            }
        }
    }
    
    int playerToMove = (components.at(1) == "w") ? WHITE : BLACK;
    bool whiteCanKCastle = (components.at(2).find("K") != string::npos);
    bool whiteCanQCastle = (components.at(2).find("Q") != string::npos);
    bool blackCanKCastle = (components.at(2).find("k") != string::npos);
    bool blackCanQCastle = (components.at(2).find("q") != string::npos);
    
    uint64_t whiteEPCaptureSq = 0, blackEPCaptureSq = 0;
    if (components.at(3).find("6") != string::npos)
        whiteEPCaptureSq = MOVEMASK[8 * (5 - 1) + (components.at(3).at(0) - 'a')];
    if (components.at(3).find("3") != string::npos)
        blackEPCaptureSq = MOVEMASK[8 * (4 - 1) + (components.at(3).at(0) - 'a')];
    int fiftyMoveCounter = stoi(components.at(4));
    int moveNumber = stoi(components.at(5));
    Board board = Board(mailbox, whiteCanKCastle, blackCanKCastle, whiteCanQCastle,
            blackCanQCastle, whiteEPCaptureSq, blackEPCaptureSq, fiftyMoveCounter,
            moveNumber, playerToMove);
    return board;
}

int main() {
    string input;
    vector<string> inputVector;
    string name = "UCI Chess Engine";
    string version = "0";
    string author = "Jeffrey An and Michael An";
    string pos;
    
    const string STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    Board board = fenToBoard(STARTPOS);
    
    cout << name << " " << version << " by " << author << '\n';
    
    while (input != "quit") {
        getline(cin, input);
        inputVector = split(input, ' ');
        
        if (input == "uci") {
            cout << "id name " << name << " " << version << '\n';
            cout << "id author " << author << '\n';
            // make variables for default, min, and max values for hash in MB
            cout << "option name Hash type spin default " << 16 << " min " << 1 << " max " << 1024 << '\n';
            cout << "uciok\n";
        }
        
        if (input == "isready") {
            // return "readyok" when all initialization of values is done
            // must return "readyok" even when searching
            cout << "readyok\n";
        }
        
        if (input == "ucinewgame") {
            // reset search
            cerr << "ucinewgame works\n";
        }
        
        if (input.substr(0, 8) == "position") {
            if (input.find("startpos") != string::npos)
                pos = STARTPOS;
            
            if (input.find("fen") != string::npos) {
                pos = inputVector.at(2) + ' ' + inputVector.at(3) + ' ' + inputVector.at(4) + ' '
                        + inputVector.at(5) + ' ' + inputVector.at(6) + ' ' + inputVector.at(7);
            }
            
            board = fenToBoard(pos);
            
            if (input.find("moves") != string::npos) {
                string moveList = input.substr(input.find("moves") + 6);
                vector<string> moveVector = split(moveList, ' ');
                
                for (unsigned i = 0; i < moveVector.size(); i++) {
                    // moveString contains the move in long algebraic notation
                    string moveString = moveVector.at(i);
                    char startFile = moveString.at(0);
                    char startRank = moveString.at(1);
                    char endFile = moveString.at(2);
                    char endRank = moveString.at(3);
                    
                    int startsq = 8 * (startRank - '0' - 1) + (startFile - 'a');
                    int endsq = 8 * (endRank - '0' - 1) + (endFile - 'a');
                    
                    int color = board.getPlayerToMove();
                    int piece = board.getMailbox()[startsq] - color;
                    
                    bool isCapture = ((board.getMailbox()[endsq] != -1)
                            || (piece == PAWNS && abs(abs(startsq - endsq) - 8) == 1));
                    bool isCastle = (piece == KINGS && abs(endsq - startsq) == 2);
                    
                    int promotion = -1;
                    
                    if (moveString.length() == 5) {
                        switch (moveString.at(4)) {
                            case 'n':
                                promotion = KNIGHTS;
                                break;
                            case 'b':
                                promotion = BISHOPS;
                                break;
                            case 'r':
                                promotion = ROOKS;
                                break;
                            case 'q':
                                promotion = QUEENS;
                                break;
                        }
                    }
                    
                    Move m(piece, isCapture, startsq, endsq);
                    
                    m.isCastle = isCastle;
                    m.promotion = promotion;
                    
                    board.doMove(&m, board.getPlayerToMove());
                }
            }
        }
        
        if (input.substr(0, 2) == "go") {
            int mode = DEPTH, value = 1;
            
            if (input.find("movetime") != string::npos && inputVector.size() > 2) {
                mode = TIME;
                value = stoi(inputVector.at(2));
            }
            
            if (input.find("depth") != string::npos && inputVector.size() > 2) {
                mode = DEPTH;
                value = stoi(inputVector.at(2));
            }
            
            if (input.find("infinite") != string::npos) {
                mode = DEPTH;
                value = MAX_DEPTH;
            }
            
            if (input.find("wtime") != string::npos) {
                mode = TIME;
                int color = board.getPlayerToMove();
                
                if (inputVector.size() == 5) {
                    if (color == 1) value = stoi(inputVector.at(2));
                    else value = stoi(inputVector.at(4));
                }
                if (inputVector.size() == 9) {
                    if (color == 1) value = stoi(inputVector.at(2)) + 40 * stoi(inputVector.at(6));
                    else value = stoi(inputVector.at(4)) + 40 * stoi(inputVector.at(8));
                }
                // Primitive time management: use at most 1/40 of remaining time with a 200 ms buffer zone
                value = (value - 200) / 40;
            }
            
            Move *bestmove = getBestMove(&board, mode, value);
            cout << "bestmove " << bestmove->toString() << '\n';
        }
        
        if (input == "stop") {
            // must stop search
            cerr << "stop works\n";
        }
        
        if (input == "mailbox") {
            for (unsigned i = 0; i < 64; i++) {
                cerr << board.getMailbox()[i] << ' ';
                if (i % 8 == 7) cerr << '\n';
            }
        }
    }
}
