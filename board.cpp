/*
    Laser, a UCI chess engine written in C++11.
    Copyright 2015-2016 Jeffrey An and Michael An

    Laser is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Laser is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Laser.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <iostream>
#include <random>
#include "board.h"
#include "btables.h"
#include "eval.h"

/**
 * @brief Our implementation of a xorshift generator as discovered by George
 * Marsaglia.
 * This specific implementation is not fully pseudorandom, but attempts to
 * create good magic number candidates by artifically increasing the number
 * of high bits.
 */
static uint64_t mseed = 0, mstate = 0;
uint64_t magicRNG() {
    // Use "y" to achieve a larger number of high bits
    uint64_t y = ((mstate << 57) | (mseed << 57)) >> 1;
    mstate ^= mseed >> 17;
    mstate ^= mstate << 3;

    uint64_t temp = mseed;
    mseed = mstate;
    mstate = temp;

    // But not too high, or they will overflow out once multiplied by the mask
    return (y | (mseed ^ mstate)) >> 1;
}

// Zobrist hashing table and the start position key, both initialized at startup
static uint64_t zobristTable[794];
static uint64_t startPosZobristKey = 0;

// Shift amounts for Dumb7fill
const int NORTH_SOUTH_FILL = 8;
const int EAST_WEST_FILL = 1;
const int NE_SW_FILL = 9;
const int NW_SE_FILL = 7;

// Dumb7fill methods, only used to initialize magic bitboard tables
uint64_t fillRayRight(uint64_t rayPieces, uint64_t empty, int shift);
uint64_t fillRayLeft(uint64_t rayPieces, uint64_t empty, int shift);

/**
 * @brief Stores the 4 values necessary to get a magic ray attack from a
 * specific square
 * @var table A pointer to the start of the array of attack sets for this square
 * @var mask The mask of relevant occupancy bits for this square
 * @var magic The magic 64-bit integer that maps the mask to the array index
 * @var shift The amount to shift by after multiplying mask by magic
 */
struct MagicInfo {
    uint64_t *table;
    uint64_t mask;
    uint64_t magic;
    int shift;
};

// Masks the relevant rook or bishop occupancy bits for magic bitboards
static uint64_t ROOK_MASK[64];
static uint64_t BISHOP_MASK[64];
// The full attack table containing all attack sets of bishops and rooks
static uint64_t *attackTable;
// The magic values for bishops, one for each square
static MagicInfo magicBishops[64];
// The magic values for rooks, one for each square
static MagicInfo magicRooks[64];

// Lookup table for all squares in a line between the from and to squares
static uint64_t inBetweenSqs[64][64];

uint64_t indexToMask64(int index, int nBits, uint64_t mask);
uint64_t ratt(int sq, uint64_t block);
uint64_t batt(int sq, uint64_t block);
int magicMap(uint64_t masked, uint64_t magic, int nBits);
uint64_t findMagic(int sq, int m, bool isBishop);

/**
 * @brief Initializes the tables and values necessary for magic bitboards.
 * We use the "fancy" approach.
 * https://chessprogramming.wikispaces.com/Magic+Bitboards
 */
void initMagicTables(uint64_t seed) {
    // An arbitrarily chosen random number generator and seed
    // The constant seed allows this process to be deterministic for optimization
    // and debugging.
    mstate = 74036198046ULL;
    mseed = seed;
    // Initialize the rook and bishop masks
    for (int i = 0; i < 64; i++) {
        // The relevant bits are everything except the edges
        // However, we don't want to remove the edge that we are on
        uint64_t relevantBits = ((~FILES[0] & ~FILES[7]) | FILES[i&7])
                              & ((~RANKS[0] & ~RANKS[7]) | RANKS[i>>3]);
        // The masks are rook and bishop attacks on an empty board
        ROOK_MASK[i] = ratt(i, 0) & relevantBits;
        BISHOP_MASK[i] = batt(i, 0) & relevantBits;
    }
    // The attack table has 107648 entries, found by summing the 2^(# relevant bits)
    // for all squares of both bishops and rooks
    attackTable = new uint64_t[107648];
    // Keeps track of the start location of attack set arrays
    int runningPtrLoc = 0;
    // Initialize bishop magic values
    for (int i = 0; i < 64; i++) {
        uint64_t *tableStart = attackTable;
        magicBishops[i].table = tableStart + runningPtrLoc;
        magicBishops[i].mask = BISHOP_MASK[i];
        magicBishops[i].magic = findMagic(i, NUM_BISHOP_BITS[i], true);
        magicBishops[i].shift = 64 - NUM_BISHOP_BITS[i];
        // We need 2^n array slots for a mask of n bits
        runningPtrLoc += 1 << NUM_BISHOP_BITS[i];
    }
    // Initialize rook magic values
    for (int i = 0; i < 64; i++) {
        uint64_t *tableStart = attackTable;
        magicRooks[i].table = tableStart + runningPtrLoc;
        magicRooks[i].mask = ROOK_MASK[i];
        magicRooks[i].magic = findMagic(i, NUM_ROOK_BITS[i], false);
        magicRooks[i].shift = 64 - NUM_ROOK_BITS[i];
        runningPtrLoc += 1 << NUM_ROOK_BITS[i];
    }
    // Set up the actual attack table, bishops first
    for (int sq = 0; sq < 64; sq++) {
        int nBits = NUM_BISHOP_BITS[sq];
        uint64_t mask = BISHOP_MASK[sq];
        // For each possible mask result
        for (int i = 0; i < (1 << nBits); i++) {
            // Find the pointer of where to store the attack sets
            uint64_t *attTableLoc = magicBishops[sq].table;
            // Find the actual masked bits from the mask index
            uint64_t occ = indexToMask64(i, nBits, mask);
            // Get the attack set for this masked occupancy
            uint64_t attSet = batt(sq, occ);
            // Do the mapping to get the location in the attack table where we
            // store the attack set
            int magicIndex = magicMap(occ, magicBishops[sq].magic, nBits);
            attTableLoc[magicIndex] = attSet;
        }
    }
    // Then rooks
    for (int sq = 0; sq < 64; sq++) {
        int nBits = NUM_ROOK_BITS[sq];
        uint64_t mask = ROOK_MASK[sq];
        for (int i = 0; i < (1 << nBits); i++) {
            uint64_t *attTableLoc = magicRooks[sq].table;
            uint64_t occ = indexToMask64(i, nBits, mask);
            uint64_t attSet = ratt(sq, occ);
            int magicIndex = magicMap(occ, magicRooks[sq].magic, nBits);
            attTableLoc[magicIndex] = attSet;
        }
    }
}

void initZobristTable() {
    std::mt19937_64 rng (61280152908);
    for (int i = 0; i < 794; i++)
        zobristTable[i] = rng();

    Board b;
    int *mailbox = b.getMailbox();
    b.initZobristKey(mailbox);
    startPosZobristKey = b.getZobristKey();
    delete[] mailbox;
}

// Initializes the 64x64 table, indexed by from and to square, of all
// squares in a line between from and to
void initInBetweenTable() {
    for (int sq1 = 0; sq1 < 64; sq1++) {
        for (int sq2 = 0; sq2 < 64; sq2++) {
            // Check horizontal/vertical lines
            uint64_t imaginaryRook = ratt(sq1, INDEX_TO_BIT[sq2]);
            // If the two squares are on a line
            if (imaginaryRook & INDEX_TO_BIT[sq2]) {
                uint64_t imaginaryRook2 = ratt(sq2, INDEX_TO_BIT[sq1]);
                // Set the bitboard of squares in between
                inBetweenSqs[sq1][sq2] = imaginaryRook & imaginaryRook2;
            }
            else {
                // Check diagonal lines
                uint64_t imaginaryBishop = batt(sq1, INDEX_TO_BIT[sq2]);
                if (imaginaryBishop & INDEX_TO_BIT[sq2]) {
                    uint64_t imaginaryBishop2 = batt(sq2, INDEX_TO_BIT[sq1]);
                    inBetweenSqs[sq1][sq2] = imaginaryBishop & imaginaryBishop2;
                }
                // If the squares are not on a line, the bitboard is empty
                else
                    inBetweenSqs[sq1][sq2] = 0;
            }
        }
    }
}

/*
 * Performs a PERFT (performance test). Useful for testing/debugging
 * PERFT n counts the number of possible positions after n moves by either side,
 * ex. PERFT 4 = # of positions after 2 moves from each side
 * 
 * 7/8/15: PERFT 5, 1.46 s (i5-2450m)
 * 7/11/15: PERFT 5, 1.22 s (i5-2450m)
 * 7/13/15: PERFT 5, 1.08 s (i5-2450m)
 * 7/14/15: PERFT 5, 0.86 s (i5-2450m)
 * 7/17/15: PERFT 5, 0.32 s (i5-2450m)
 * 8/7/15: PERFT 5, 0.25 s, PERFT 6, 6.17 s (i5-5200u)
 * 8/8/15: PERFT 6, 5.90 s (i5-5200u)
 * 8/11/15: PERFT 6, 5.20 s (i5-5200u)
 */
uint64_t perft(Board &b, int color, int depth, uint64_t &captures) {
    if (depth == 0)
        return 1;

    uint64_t nodes = 0;

    PieceMoveList pml = b.getPieceMoveList<PML_LEGAL_MOVES>(color);
    MoveList pl = b.getAllPseudoLegalMoves(color, pml);
    for (unsigned int i = 0; i < pl.size(); i++) {
        Board copy = b.staticCopy();
        if (!copy.doPseudoLegalMove(pl.get(i), color))
            continue;

        if (isCapture(pl.get(i)))
            captures++;

        nodes += perft(copy, color^1, depth-1, captures);
    }

    return nodes;
}


//------------------------------------------------------------------------------
//--------------------------------Constructors----------------------------------
//------------------------------------------------------------------------------

// Create a board object initialized to the start position.
Board::Board() {
    allPieces[WHITE] = 0x000000000000FFFF;
    allPieces[BLACK] = 0xFFFF000000000000;
    pieces[WHITE][PAWNS] = 0x000000000000FF00; // white pawns
    pieces[WHITE][KNIGHTS] = 0x0000000000000042; // white knights
    pieces[WHITE][BISHOPS] = 0x0000000000000024; // white bishops
    pieces[WHITE][ROOKS] = 0x0000000000000081; // white rooks
    pieces[WHITE][QUEENS] = 0x0000000000000008; // white queens
    pieces[WHITE][KINGS] = 0x0000000000000010; // white kings
    pieces[BLACK][PAWNS] = 0x00FF000000000000; // black pawns
    pieces[BLACK][KNIGHTS] = 0x4200000000000000; // black knights
    pieces[BLACK][BISHOPS] = 0x2400000000000000; // black bishops
    pieces[BLACK][ROOKS] = 0x8100000000000000; // black rooks
    pieces[BLACK][QUEENS] = 0x0800000000000000; // black queens
    pieces[BLACK][KINGS] = 0x1000000000000000; // black kings

    zobristKey = startPosZobristKey;
    epCaptureFile = NO_EP_POSSIBLE;
    playerToMove = WHITE;
    moveNumber = 1;
    castlingRights = WHITECASTLE | BLACKCASTLE;
    fiftyMoveCounter = 0;
}

// Create a board object from a mailbox of the current board state.
Board::Board(int *mailboxBoard, bool _whiteCanKCastle, bool _blackCanKCastle,
        bool _whiteCanQCastle, bool _blackCanQCastle,  uint16_t _epCaptureFile,
        int _fiftyMoveCounter, int _moveNumber, int _playerToMove) {
    // Initialize bitboards
    for (int i = 0; i < 12; i++) {
        pieces[i/6][i%6] = 0;
    }
    for (int i = 0; i < 64; i++) {
        if (0 <= mailboxBoard[i] && mailboxBoard[i] <= 11) {
            pieces[mailboxBoard[i]/6][mailboxBoard[i]%6] |= INDEX_TO_BIT[i];
        }
    }
    allPieces[WHITE] = 0;
    for (int i = 0; i < 6; i++)
        allPieces[WHITE] |= pieces[WHITE][i];
    allPieces[BLACK] = 0;
    for (int i = 0; i < 6; i++)
        allPieces[BLACK] |= pieces[BLACK][i];

    epCaptureFile = _epCaptureFile;
    playerToMove = _playerToMove;
    moveNumber = _moveNumber;
    castlingRights = 0;
    if (_whiteCanKCastle)
        castlingRights |= WHITEKSIDE;
    if (_whiteCanQCastle)
        castlingRights |= WHITEQSIDE;
    if (_blackCanKCastle)
        castlingRights |= BLACKKSIDE;
    if (_blackCanQCastle)
        castlingRights |= BLACKQSIDE;
    fiftyMoveCounter = _fiftyMoveCounter;
    initZobristKey(mailboxBoard);
}

Board::~Board() {}

// Creates a copy of a board
// Private constructor used only with staticCopy()
Board::Board(Board *b) {
    allPieces[WHITE] = b->allPieces[WHITE];
    allPieces[BLACK] = b->allPieces[BLACK];
    for (int i = 0; i < 6; i++) {
        pieces[0][i] = b->pieces[0][i];
    }
    for (int i = 0; i < 6; i++) {
        pieces[1][i] = b->pieces[1][i];
    }

    zobristKey = b->zobristKey;
    epCaptureFile = b->epCaptureFile;
    playerToMove = b->playerToMove;
    moveNumber = b->moveNumber;
    castlingRights = b->castlingRights;
    fiftyMoveCounter = b->fiftyMoveCounter;
}

Board Board::staticCopy() {
    return Board(this);
}


//------------------------------------------------------------------------------
//---------------------------------Do Move--------------------------------------
//------------------------------------------------------------------------------

/**
 * @brief Updates the board and Zobrist keys with Move m.
 */
void Board::doMove(Move m, int color) {
    int startSq = getStartSq(m);
    int endSq = getEndSq(m);
    int pieceID = getPieceOnSquare(color, startSq);

    // Update flag based elements of Zobrist key
    zobristKey ^= zobristTable[769 + castlingRights];
    zobristKey ^= zobristTable[785 + epCaptureFile];


    if (isPromotion(m)) {
        int promotionType = getPromotion(m);
        if (isCapture(m)) {
            int captureType = getPieceOnSquare(color^1, endSq);
            pieces[color][PAWNS] &= ~INDEX_TO_BIT[startSq];
            pieces[color][promotionType] |= INDEX_TO_BIT[endSq];
            pieces[color^1][captureType] &= ~INDEX_TO_BIT[endSq];

            allPieces[color] &= ~INDEX_TO_BIT[startSq];
            allPieces[color] |= INDEX_TO_BIT[endSq];
            allPieces[color^1] &= ~INDEX_TO_BIT[endSq];

            zobristKey ^= zobristTable[384*color + startSq];
            zobristKey ^= zobristTable[384*color + 64*promotionType + endSq];
            zobristKey ^= zobristTable[384*(color^1) + 64*captureType + endSq];
        }
        else {
            pieces[color][PAWNS] &= ~INDEX_TO_BIT[startSq];
            pieces[color][promotionType] |= INDEX_TO_BIT[endSq];

            allPieces[color] &= ~INDEX_TO_BIT[startSq];
            allPieces[color] |= INDEX_TO_BIT[endSq];

            zobristKey ^= zobristTable[384*color + startSq];
            zobristKey ^= zobristTable[384*color + 64*promotionType + endSq];
        }
        epCaptureFile = NO_EP_POSSIBLE;
        fiftyMoveCounter = 0;
    } // end promotion
    else if (isCapture(m)) {
        if (isEP(m)) {
            pieces[color][PAWNS] &= ~INDEX_TO_BIT[startSq];
            pieces[color][PAWNS] |= INDEX_TO_BIT[endSq];
            uint64_t epCaptureSq = INDEX_TO_BIT[epVictimSquare(color^1, epCaptureFile)];
            pieces[color^1][PAWNS] &= ~epCaptureSq;

            allPieces[color] &= ~INDEX_TO_BIT[startSq];
            allPieces[color] |= INDEX_TO_BIT[endSq];
            allPieces[color^1] &= ~epCaptureSq;

            int capSq = epVictimSquare(color^1, epCaptureFile);
            zobristKey ^= zobristTable[384*color + startSq];
            zobristKey ^= zobristTable[384*color + endSq];
            zobristKey ^= zobristTable[384*(color^1) + capSq];
        }
        else {
            int captureType = getPieceOnSquare(color^1, endSq);
            pieces[color][pieceID] &= ~INDEX_TO_BIT[startSq];
            pieces[color][pieceID] |= INDEX_TO_BIT[endSq];
            pieces[color^1][captureType] &= ~INDEX_TO_BIT[endSq];

            allPieces[color] &= ~INDEX_TO_BIT[startSq];
            allPieces[color] |= INDEX_TO_BIT[endSq];
            allPieces[color^1] &= ~INDEX_TO_BIT[endSq];

            zobristKey ^= zobristTable[384*color + 64*pieceID + startSq];
            zobristKey ^= zobristTable[384*color + 64*pieceID + endSq];
            zobristKey ^= zobristTable[384*(color^1) + 64*captureType + endSq];
        }
        epCaptureFile = NO_EP_POSSIBLE;
        fiftyMoveCounter = 0;
    } // end capture
    else { // Quiet moves
        if (isCastle(m)) {
            if (endSq == 6) { // white kside
                pieces[WHITE][KINGS] &= ~INDEX_TO_BIT[4];
                pieces[WHITE][KINGS] |= INDEX_TO_BIT[6];
                pieces[WHITE][ROOKS] &= ~INDEX_TO_BIT[7];
                pieces[WHITE][ROOKS] |= INDEX_TO_BIT[5];

                allPieces[WHITE] &= ~INDEX_TO_BIT[4];
                allPieces[WHITE] |= INDEX_TO_BIT[6];
                allPieces[WHITE] &= ~INDEX_TO_BIT[7];
                allPieces[WHITE] |= INDEX_TO_BIT[5];

                zobristKey ^= zobristTable[64*KINGS+4];
                zobristKey ^= zobristTable[64*KINGS+6];
                zobristKey ^= zobristTable[64*ROOKS+7];
                zobristKey ^= zobristTable[64*ROOKS+5];
            }
            else if (endSq == 2) { // white qside
                pieces[WHITE][KINGS] &= ~INDEX_TO_BIT[4];
                pieces[WHITE][KINGS] |= INDEX_TO_BIT[2];
                pieces[WHITE][ROOKS] &= ~INDEX_TO_BIT[0];
                pieces[WHITE][ROOKS] |= INDEX_TO_BIT[3];

                allPieces[WHITE] &= ~INDEX_TO_BIT[4];
                allPieces[WHITE] |= INDEX_TO_BIT[2];
                allPieces[WHITE] &= ~INDEX_TO_BIT[0];
                allPieces[WHITE] |= INDEX_TO_BIT[3];

                zobristKey ^= zobristTable[64*KINGS+4];
                zobristKey ^= zobristTable[64*KINGS+2];
                zobristKey ^= zobristTable[64*ROOKS+0];
                zobristKey ^= zobristTable[64*ROOKS+3];
            }
            else if (endSq == 62) { // black kside
                pieces[BLACK][KINGS] &= ~INDEX_TO_BIT[60];
                pieces[BLACK][KINGS] |= INDEX_TO_BIT[62];
                pieces[BLACK][ROOKS] &= ~INDEX_TO_BIT[63];
                pieces[BLACK][ROOKS] |= INDEX_TO_BIT[61];

                allPieces[BLACK] &= ~INDEX_TO_BIT[60];
                allPieces[BLACK] |= INDEX_TO_BIT[62];
                allPieces[BLACK] &= ~INDEX_TO_BIT[63];
                allPieces[BLACK] |= INDEX_TO_BIT[61];

                zobristKey ^= zobristTable[384+64*KINGS+60];
                zobristKey ^= zobristTable[384+64*KINGS+62];
                zobristKey ^= zobristTable[384+64*ROOKS+63];
                zobristKey ^= zobristTable[384+64*ROOKS+61];
            }
            else { // black qside
                pieces[BLACK][KINGS] &= ~INDEX_TO_BIT[60];
                pieces[BLACK][KINGS] |= INDEX_TO_BIT[58];
                pieces[BLACK][ROOKS] &= ~INDEX_TO_BIT[56];
                pieces[BLACK][ROOKS] |= INDEX_TO_BIT[59];

                allPieces[BLACK] &= ~INDEX_TO_BIT[60];
                allPieces[BLACK] |= INDEX_TO_BIT[58];
                allPieces[BLACK] &= ~INDEX_TO_BIT[56];
                allPieces[BLACK] |= INDEX_TO_BIT[59];

                zobristKey ^= zobristTable[384+64*KINGS+60];
                zobristKey ^= zobristTable[384+64*KINGS+58];
                zobristKey ^= zobristTable[384+64*ROOKS+56];
                zobristKey ^= zobristTable[384+64*ROOKS+59];
            }
            epCaptureFile = NO_EP_POSSIBLE;
            fiftyMoveCounter++;
        } // end castling
        else { // other quiet moves
            pieces[color][pieceID] &= ~INDEX_TO_BIT[startSq];
            pieces[color][pieceID] |= INDEX_TO_BIT[endSq];

            allPieces[color] &= ~INDEX_TO_BIT[startSq];
            allPieces[color] |= INDEX_TO_BIT[endSq];

            zobristKey ^= zobristTable[384*color + 64*pieceID + startSq];
            zobristKey ^= zobristTable[384*color + 64*pieceID + endSq];

            // check for en passant
            if (pieceID == PAWNS) {
                if (getFlags(m) == MOVE_DOUBLE_PAWN)
                    epCaptureFile = startSq & 7;
                else
                    epCaptureFile = NO_EP_POSSIBLE;

                fiftyMoveCounter = 0;
            }
            else {
                epCaptureFile = NO_EP_POSSIBLE;
                fiftyMoveCounter++;
            }
        } // end other quiet moves
    } // end normal move

    // change castling flags
    if (pieceID == KINGS) {
        if (color == WHITE)
            castlingRights &= ~WHITECASTLE;
        else
            castlingRights &= ~BLACKCASTLE;
    }
    // Castling rights change because of the rook only when the rook moves or
    // is captured
    else if (isCapture(m) || pieceID == ROOKS) {
        // No sense in removing the rights if they're already gone
        if (castlingRights & WHITECASTLE) {
            if ((pieces[WHITE][ROOKS] & 0x80) == 0)
                castlingRights &= ~WHITEKSIDE;
            if ((pieces[WHITE][ROOKS] & 1) == 0)
                castlingRights &= ~WHITEQSIDE;
        }
        if (castlingRights & BLACKCASTLE) {
            uint64_t blackR = pieces[BLACK][ROOKS] >> 56;
            if ((blackR & 0x80) == 0)
                castlingRights &= ~BLACKKSIDE;
            if ((blackR & 0x1) == 0)
                castlingRights &= ~BLACKQSIDE;
        }
    } // end castling flags

    zobristKey ^= zobristTable[769 + castlingRights];
    zobristKey ^= zobristTable[785 + epCaptureFile];

    if (color == BLACK)
        moveNumber++;
    playerToMove = color^1;
    zobristKey ^= zobristTable[768];
}

bool Board::doPseudoLegalMove(Move m, int color) {
    doMove(m, color);
    // Pseudo-legal moves require a check for legality
    return !(isInCheck(color));
}

// Do a hash move, which requires a few more checks in case of a Type-1 error.
bool Board::doHashMove(Move m, int color) {
    int pieceID = getPieceOnSquare(color, getStartSq(m));
    // Check that the start square is not empty
    if (pieceID == -1)
        return false;

    // Check that the end square has correct occupancy
    uint64_t otherPieces = allPieces[color^1];
    uint64_t endSingle = INDEX_TO_BIT[getEndSq(m)];
    bool captureRoutes = (isCapture(m) && (otherPieces & endSingle))
                      || (isCapture(m) && pieceID == PAWNS && (~otherPieces & endSingle));
    uint64_t empty = ~getOccupancy();
    if (!(captureRoutes || (!isCapture(m) && (empty & endSingle))))
        return false;
    // Check that the king is not captured
    if (isCapture(m) && ((endSingle & pieces[WHITE][KINGS]) || (endSingle & pieces[BLACK][KINGS])))
        return false;

    return doPseudoLegalMove(m, color);
}

// Handle null moves for null move pruning by switching the player to move.
void Board::doNullMove() {
    playerToMove = playerToMove ^ 1;
    zobristKey ^= zobristTable[768];
    zobristKey ^= zobristTable[785 + epCaptureFile];
    epCaptureFile = NO_EP_POSSIBLE;
    zobristKey ^= zobristTable[785 + epCaptureFile];
}

void Board::undoNullMove(uint16_t _epCaptureFile) {
    playerToMove = playerToMove ^ 1;
    zobristKey ^= zobristTable[768];
    zobristKey ^= zobristTable[785 + epCaptureFile];
    epCaptureFile = _epCaptureFile;
    zobristKey ^= zobristTable[785 + epCaptureFile];
}


//------------------------------------------------------------------------------
//-------------------------------Move Generation--------------------------------
//------------------------------------------------------------------------------

/*
 * @brief Returns a list of structs containing the piece type, start square,
 * and a bitboard of potential legal moves for each knight, bishop, rook, and queen.
 */
template <bool isMoveGen>
PieceMoveList Board::getPieceMoveList(int color) {
    PieceMoveList pml;

    uint64_t knights = pieces[color][KNIGHTS];
    while (knights) {
        int stSq = bitScanForward(knights);
        knights &= knights-1;
        uint64_t nSq = getKnightSquares(stSq);

        pml.add(PieceMoveInfo(KNIGHTS, stSq, nSq));
    }

    uint64_t occ = isMoveGen ? getOccupancy()
                             : allPieces[color^1] | pieces[color][PAWNS] | pieces[color][KINGS];
    uint64_t bishops = pieces[color][BISHOPS];
    while (bishops) {
        int stSq = bitScanForward(bishops);
        bishops &= bishops-1;
        uint64_t bSq = getBishopSquares(stSq, occ);

        pml.add(PieceMoveInfo(BISHOPS, stSq, bSq));
    }

    uint64_t rooks = pieces[color][ROOKS];
    while (rooks) {
        int stSq = bitScanForward(rooks);
        rooks &= rooks-1;
        uint64_t rSq = getRookSquares(stSq, occ);

        pml.add(PieceMoveInfo(ROOKS, stSq, rSq));
    }

    uint64_t queens = pieces[color][QUEENS];
    while (queens) {
        int stSq = bitScanForward(queens);
        queens &= queens-1;
        uint64_t qSq = getQueenSquares(stSq, occ);

        pml.add(PieceMoveInfo(QUEENS, stSq, qSq));
    }

    return pml;
}

// Get all legal moves and captures
MoveList Board::getAllLegalMoves(int color) {
    PieceMoveList pml = getPieceMoveList<PML_LEGAL_MOVES>(color);
    MoveList moves = getAllPseudoLegalMoves(color, pml);

    for (unsigned int i = 0; i < moves.size(); i++) {
        Board b = staticCopy();
        b.doMove(moves.get(i), color);

        if (b.isInCheck(color)) {
            moves.remove(i);
            i--;
        }
    }

    return moves;
}

//------------------------------Pseudo-legal Moves------------------------------
/* Pseudo-legal moves disregard whether the player's king is left in check
 * The pseudo-legal move and capture generators all follow a similar scheme:
 * Bitscan to obtain the square number for each piece (a1 is 0, a2 is 1, h8 is 63).
 * Get the legal moves as a bitboard, then bitscan this to get the destination
 * square and store as a Move object.
 */
MoveList Board::getAllPseudoLegalMoves(int color, PieceMoveList &pml) {
    MoveList quiets = getPseudoLegalQuiets(color, pml);
    MoveList captures = getPseudoLegalCaptures(color, pml, true);

    // Put captures before quiet moves
    for (unsigned int i = 0; i < quiets.size(); i++) {
        captures.add(quiets.get(i));
    }
    return captures;
}

/*
 * Quiet moves are generated in the following order:
 * Castling
 * Knight moves
 * Bishop moves
 * Rook moves
 * Queen moves
 * Pawn moves
 * King moves
 */
MoveList Board::getPseudoLegalQuiets(int color, PieceMoveList &pml) {
    MoveList quiets;

    addCastlesToList(quiets, color);

    for (unsigned int i = 0; i < pml.size(); i++) {
        PieceMoveInfo pmi = pml.get(i);
        addMovesToList<MOVEGEN_QUIETS>(quiets, pmi.startSq, pmi.legal);
    }

    addPawnMovesToList(quiets, color);

    int stsqK = bitScanForward(pieces[color][KINGS]);
    uint64_t kingSqs = getKingSquares(stsqK);
    addMovesToList<MOVEGEN_QUIETS>(quiets, stsqK, kingSqs);

    return quiets;
}

/*
 * Do not include promotions for quiescence search, include promotions in normal search.
 * Captures are generated in the following order:
 * King captures
 * Pawn captures
 * Knight captures
 * Bishop captures
 * Rook captures
 * Queen captures
 */
MoveList Board::getPseudoLegalCaptures(int color, PieceMoveList &pml, bool includePromotions) {
    MoveList captures;

    uint64_t otherPieces = allPieces[color^1];

    int kingStSq = bitScanForward(pieces[color][KINGS]);
    uint64_t kingSqs = getKingSquares(kingStSq);
    addMovesToList<MOVEGEN_CAPTURES>(captures, kingStSq, kingSqs, otherPieces);

    addPawnCapturesToList(captures, color, otherPieces, includePromotions);

    for (unsigned int i = 0; i < pml.size(); i++) {
        PieceMoveInfo pmi = pml.get(i);
        addMovesToList<MOVEGEN_CAPTURES>(captures, pmi.startSq, pmi.legal, otherPieces);
    }

    return captures;
}

// Generates all queen promotions for quiescence search
MoveList Board::getPseudoLegalPromotions(int color) {
    MoveList moves;
    uint64_t otherPieces = allPieces[color^1];

    uint64_t pawns = pieces[color][PAWNS];
    uint64_t finalRank = (color == WHITE) ? RANKS[7] : RANKS[0];

    int leftDiff = (color == WHITE) ? -7 : 9;
    int rightDiff = (color == WHITE) ? -9 : 7;

    uint64_t legal = (color == WHITE) ? getWPawnLeftCaptures(pawns)
                                      : getBPawnLeftCaptures(pawns);
    legal &= otherPieces;
    uint64_t promotions = legal & finalRank;

    while (promotions) {
        int endSq = bitScanForward(promotions);
        promotions &= promotions-1;

        Move mq = encodeMove(endSq+leftDiff, endSq);
        mq = setCapture(mq, true);
        mq = setFlags(mq, MOVE_PROMO_Q);
        moves.add(mq);
    }

    legal = (color == WHITE) ? getWPawnRightCaptures(pawns)
                             : getBPawnRightCaptures(pawns);
    legal &= otherPieces;
    promotions = legal & finalRank;

    while (promotions) {
        int endSq = bitScanForward(promotions);
        promotions &= promotions-1;

        Move mq = encodeMove(endSq+rightDiff, endSq);
        mq = setCapture(mq, true);
        mq = setFlags(mq, MOVE_PROMO_Q);
        moves.add(mq);
    }

    int sqDiff = (color == WHITE) ? -8 : 8;

    legal = (color == WHITE) ? getWPawnSingleMoves(pawns)
                             : getBPawnSingleMoves(pawns);
    promotions = legal & finalRank;

    while (promotions) {
        int endSq = bitScanForward(promotions);
        promotions &= promotions - 1;
        int stSq = endSq + sqDiff;

        Move mq = encodeMove(stSq, endSq);
        mq = setFlags(mq, MOVE_PROMO_Q);
        moves.add(mq);
    }

    return moves;
}

/*
 * Get all pseudo-legal non-capture checks for a position. Used in quiescence search.
 * This function can be optimized compared to a normal getLegalMoves() because
 * for each piece, we can intersect the legal moves of the piece with the attack
 * map of this piece to the opposing king square to determine direct checks.
 * Discovered checks have to be handled separately.
 *
 * For simplicity, promotions and en passant are left out of this function.
 */
MoveList Board::getPseudoLegalChecks(int color) {
    MoveList checks;
    int kingSq = bitScanForward(pieces[color^1][KINGS]);
    // Square parity for knight and bishop moves
    uint64_t kingParity = (pieces[color^1][KINGS] & LIGHT) ? LIGHT : DARK;
    uint64_t potentialXRay = pieces[color][BISHOPS] | pieces[color][ROOKS] | pieces[color][QUEENS];

    // We can do pawns in parallel, since the start square of a pawn move is
    // determined by its end square.
    uint64_t pawns = pieces[color][PAWNS];

    // First, deal with discovered checks
    // TODO this is way too slow
    /*
    uint64_t tempPawns = pawns;
    while (tempPawns) {
        int stsq = bitScanForward(tempPawns);
        tempPawns &= tempPawns - 1;
        uint64_t xrays = getXRayPieceMap(color, kingSq, color, INDEX_TO_BIT[stsq]);
        // If moving the pawn caused a new xray piece to attack the king
        if (!(xrays & invAttackMap)) {
            // Every legal move of this pawn is a legal check
            uint64_t legal = (color == WHITE) ? getWPawnSingleMoves(INDEX_TO_BIT[stsq]) | getWPawnDoubleMoves(INDEX_TO_BIT[stsq])
                                              : getBPawnSingleMoves(INDEX_TO_BIT[stsq]) | getBPawnDoubleMoves(INDEX_TO_BIT[stsq]);
            while (legal) {
                int endsq = bitScanForward(legal);
                legal &= legal - 1;
                checks.add(encodeMove(stsq, endsq, PAWNS, false));
            }

            // Remove this pawn from future consideration
            pawns ^= INDEX_TO_BIT[stsq];
        }
    }
    */

    uint64_t pAttackMap = (color == WHITE) 
            ? getBPawnCaptures(INDEX_TO_BIT[kingSq])
            : getWPawnCaptures(INDEX_TO_BIT[kingSq]);
    uint64_t finalRank = (color == WHITE) ? RANKS[7] : RANKS[0];
    int sqDiff = (color == WHITE) ? -8 : 8;

    uint64_t pLegal = (color == WHITE) ? getWPawnSingleMoves(pawns)
                                       : getBPawnSingleMoves(pawns);
    // Remove promotions
    uint64_t promotions = pLegal & finalRank;
    pLegal ^= promotions;

    pLegal &= pAttackMap;
    while (pLegal) {
        int endsq = bitScanForward(pLegal);
        pLegal &= pLegal - 1;
        checks.add(encodeMove(endsq+sqDiff, endsq));
    }

    pLegal = (color == WHITE) ? getWPawnDoubleMoves(pawns)
                              : getBPawnDoubleMoves(pawns);
    pLegal &= pAttackMap;
    while (pLegal) {
        int endsq = bitScanForward(pLegal);
        pLegal &= pLegal - 1;
        Move m = encodeMove(endsq+2*sqDiff, endsq);
        m = setFlags(m, MOVE_DOUBLE_PAWN);
        checks.add(m);
    }

    uint64_t knights = pieces[color][KNIGHTS] & kingParity;
    uint64_t nAttackMap = getKnightSquares(kingSq);
    while (knights) {
        int stsq = bitScanForward(knights);
        knights &= knights-1;
        uint64_t nSq = getKnightSquares(stsq);
        // Get any bishops, rooks, queens attacking king after knight has moved
        uint64_t xrays = getXRayPieceMap(color, kingSq, color, INDEX_TO_BIT[stsq], 0);
        // If still no xrayers are giving check, then we have no discovered
        // check. Otherwise, every move by this piece is a (discovered) checking
        // move
        if (!(xrays & potentialXRay))
            nSq &= nAttackMap;

        addMovesToList<MOVEGEN_QUIETS>(checks, stsq, nSq);
    }

    uint64_t occ = getOccupancy();
    uint64_t bishops = pieces[color][BISHOPS] & kingParity;
    uint64_t bAttackMap = getBishopSquares(kingSq, occ);
    while (bishops) {
        int stsq = bitScanForward(bishops);
        bishops &= bishops-1;
        uint64_t bSq = getBishopSquares(stsq, occ);
        uint64_t xrays = getXRayPieceMap(color, kingSq, color, INDEX_TO_BIT[stsq], 0);
        if (!(xrays & potentialXRay))
            bSq &= bAttackMap;

        addMovesToList<MOVEGEN_QUIETS>(checks, stsq, bSq);
    }

    uint64_t rooks = pieces[color][ROOKS];
    uint64_t rAttackMap = getRookSquares(kingSq, occ);
    while (rooks) {
        int stsq = bitScanForward(rooks);
        rooks &= rooks-1;
        uint64_t rSq = getRookSquares(stsq, occ);
        uint64_t xrays = getXRayPieceMap(color, kingSq, color, INDEX_TO_BIT[stsq], 0);
        if (!(xrays & potentialXRay))
            rSq &= rAttackMap;

        addMovesToList<MOVEGEN_QUIETS>(checks, stsq, rSq);
    }

    uint64_t queens = pieces[color][QUEENS];
    uint64_t qAttackMap = getQueenSquares(kingSq, occ);
    while (queens) {
        int stsq = bitScanForward(queens);
        queens &= queens-1;
        uint64_t qSq = getQueenSquares(stsq, occ) & qAttackMap;

        addMovesToList<MOVEGEN_QUIETS>(checks, stsq, qSq);
    }

    return checks;
}

// Generate moves that (sort of but not really) get out of check
// This can only be used if we know the side to move is in check
// Optimizations include looking for double check (king moves only),
// otherwise we can only capture the checker or block if it is an xray piece
MoveList Board::getPseudoLegalCheckEscapes(int color, PieceMoveList &pml) {
    MoveList captures, blocks;

    int kingSq = bitScanForward(pieces[color][KINGS]);
    uint64_t otherPieces = allPieces[color^1];
    uint64_t attackMap = getAttackMap(color^1, kingSq);
    // Consider only captures of pieces giving check
    otherPieces &= attackMap;

    // If double check, we can only move the king
    if (count(otherPieces) >= 2) {
        uint64_t kingSqs = getKingSquares(kingSq);

        addMovesToList<MOVEGEN_CAPTURES>(captures, kingSq, kingSqs, allPieces[color^1]);
        addMovesToList<MOVEGEN_QUIETS>(captures, kingSq, kingSqs);
        return captures;
    }

    addPawnMovesToList(blocks, color);
    addPawnCapturesToList(captures, color, otherPieces, true);

    uint64_t occ = getOccupancy();
    // If bishops, rooks, or queens, get bitboard of attack path so we
    // can intersect with legal moves to get legal block moves
    uint64_t xraySqs = 0;
    int attackerSq = bitScanForward(otherPieces);
    int attackerType = getPieceOnSquare(color^1, attackerSq);
    if (attackerType == BISHOPS)
        xraySqs = getBishopSquares(attackerSq, occ);
    else if (attackerType == ROOKS)
        xraySqs = getRookSquares(attackerSq, occ);
    else if (attackerType == QUEENS)
        xraySqs = getQueenSquares(attackerSq, occ);

    for (unsigned int i = 0; i < pml.size(); i++) {
        PieceMoveInfo pmi = pml.get(i);
        addMovesToList<MOVEGEN_QUIETS>(blocks, pmi.startSq, pmi.legal & xraySqs);
        addMovesToList<MOVEGEN_CAPTURES>(captures, pmi.startSq, pmi.legal, otherPieces);
    }

    int stsqK = bitScanForward(pieces[color][KINGS]);
    uint64_t kingSqs = getKingSquares(stsqK);
    addMovesToList<MOVEGEN_QUIETS>(blocks, stsqK, kingSqs);
    addMovesToList<MOVEGEN_CAPTURES>(captures, stsqK, kingSqs, allPieces[color^1]);

    // Put captures before blocking moves
    for (unsigned int i = 0; i < blocks.size(); i++) {
        captures.add(blocks.get(i));
    }

    return captures;
}

//------------------------------------------------------------------------------
//---------------------------Move Generation Helpers----------------------------
//------------------------------------------------------------------------------
// We can do pawns in parallel, since the start square of a pawn move is
// determined by its end square.
void Board::addPawnMovesToList(MoveList &quiets, int color) {
    uint64_t pawns = pieces[color][PAWNS];
    uint64_t finalRank = (color == WHITE) ? RANKS[7] : RANKS[0];
    int sqDiff = (color == WHITE) ? -8 : 8;

    uint64_t pLegal = (color == WHITE) ? getWPawnSingleMoves(pawns)
                                       : getBPawnSingleMoves(pawns);
    // Promotions occur when a pawn reaches the final rank
    uint64_t promotions = pLegal & finalRank;
    pLegal ^= promotions;

    while (promotions) {
        int endSq = bitScanForward(promotions);
        promotions &= promotions - 1;
        int stSq = endSq + sqDiff;

        addPromotionsToList<MOVEGEN_QUIETS>(quiets, stSq, endSq);
    }
    while (pLegal) {
        int endsq = bitScanForward(pLegal);
        pLegal &= pLegal - 1;
        quiets.add(encodeMove(endsq+sqDiff, endsq));
    }

    pLegal = (color == WHITE) ? getWPawnDoubleMoves(pawns)
                              : getBPawnDoubleMoves(pawns);
    while (pLegal) {
        int endsq = bitScanForward(pLegal);
        pLegal &= pLegal - 1;
        Move m = encodeMove(endsq+2*sqDiff, endsq);
        m = setFlags(m, MOVE_DOUBLE_PAWN);
        quiets.add(m);
    }
}

// For pawn captures, we can use a similar approach, but we must consider
// left-hand and right-hand captures separately so we can tell which
// pawn is doing the capturing.
void Board::addPawnCapturesToList(MoveList &captures, int color, uint64_t otherPieces, bool includePromotions) {
    uint64_t pawns = pieces[color][PAWNS];
    uint64_t finalRank = (color == WHITE) ? RANKS[7] : RANKS[0];
    int leftDiff = (color == WHITE) ? -7 : 9;
    int rightDiff = (color == WHITE) ? -9 : 7;

    uint64_t legal = (color == WHITE) ? getWPawnLeftCaptures(pawns)
                                      : getBPawnLeftCaptures(pawns);
    legal &= otherPieces;
    uint64_t promotions = legal & finalRank;
    legal ^= promotions;

    if (includePromotions) {
        while (promotions) {
            int endSq = bitScanForward(promotions);
            promotions &= promotions-1;

            addPromotionsToList<MOVEGEN_CAPTURES>(captures, endSq+leftDiff, endSq);
        }
    }
    while (legal) {
        int endsq = bitScanForward(legal);
        legal &= legal-1;
        Move m = encodeMove(endsq+leftDiff, endsq);
        m = setCapture(m, true);
        captures.add(m);
    }

    legal = (color == WHITE) ? getWPawnRightCaptures(pawns)
                             : getBPawnRightCaptures(pawns);
    legal &= otherPieces;
    promotions = legal & finalRank;
    legal ^= promotions;

    if (includePromotions) {
        while (promotions) {
            int endSq = bitScanForward(promotions);
            promotions &= promotions-1;

            addPromotionsToList<MOVEGEN_CAPTURES>(captures, endSq+rightDiff, endSq);
        }
    }
    while (legal) {
        int endsq = bitScanForward(legal);
        legal &= legal-1;
        Move m = encodeMove(endsq+rightDiff, endsq);
        m = setCapture(m, true);
        captures.add(m);
    }

    // If there are en passants possible...
    if (epCaptureFile != NO_EP_POSSIBLE) {
        int victimSq = epVictimSquare(color^1, epCaptureFile);
        // capturer's destination square is either the rank above (white) or
        // below (black) the victim square
        int rankDiff = (color == WHITE) ? 8 : -8;
        // The capturer's start square is either 1 to the left or right of victim
        if ((INDEX_TO_BIT[victimSq] << 1) & NOTA & pieces[color][PAWNS]) {
            Move m = encodeMove(victimSq+1, victimSq+rankDiff);
            m = setFlags(m, MOVE_EP);
            captures.add(m);
        }
        if ((INDEX_TO_BIT[victimSq] >> 1) & NOTH & pieces[color][PAWNS]) {
            Move m = encodeMove(victimSq-1, victimSq+rankDiff);
            m = setFlags(m, MOVE_EP);
            captures.add(m);
        }
    }
}

// Helper function that processes a bitboard of legal moves and adds all
// moves into a list.
template <bool isCapture>
void Board::addMovesToList(MoveList &moves, int stSq, uint64_t allEndSqs,
    uint64_t otherPieces) {

    uint64_t intersect = (isCapture) ? otherPieces : ~getOccupancy();
    uint64_t legal = allEndSqs & intersect;
    while (legal) {
        int endSq = bitScanForward(legal);
        legal &= legal-1;
        Move m = encodeMove(stSq, endSq);
        if (isCapture)
            m = setCapture(m, true);
        moves.add(m);
    }
}

template <bool isCapture>
void Board::addPromotionsToList(MoveList &moves, int stSq, int endSq) {
    Move mk = encodeMove(stSq, endSq);
    mk = setFlags(mk, MOVE_PROMO_N);
    Move mb = encodeMove(stSq, endSq);
    mb = setFlags(mb, MOVE_PROMO_B);
    Move mr = encodeMove(stSq, endSq);
    mr = setFlags(mr, MOVE_PROMO_R);
    Move mq = encodeMove(stSq, endSq);
    mq = setFlags(mq, MOVE_PROMO_Q);
    if (isCapture) {
        mk = setCapture(mk, true);
        mb = setCapture(mb, true);
        mr = setCapture(mr, true);
        mq = setCapture(mq, true);
    }
    // Order promotions queen, knight, rook, bishop
    moves.add(mq);
    moves.add(mk);
    moves.add(mr);
    moves.add(mb);
}

void Board::addCastlesToList(MoveList &moves, int color) {
    // Add all possible castles
    if (color == WHITE) {
        // If castling rights still exist, squares in between king and rook are
        // empty, and player is not in check
        if ((castlingRights & WHITEKSIDE)
         && (getOccupancy() & WHITE_KSIDE_PASSTHROUGH_SQS) == 0
         && !isInCheck(WHITE)) {
            // Check for castling through check
            if (getAttackMap(BLACK, 5) == 0) {
                Move m = encodeMove(4, 6);
                m = setCastle(m, true);
                moves.add(m);
            }
        }
        if ((castlingRights & WHITEQSIDE)
         && (getOccupancy() & WHITE_QSIDE_PASSTHROUGH_SQS) == 0
         && !isInCheck(WHITE)) {
            if (getAttackMap(BLACK, 3) == 0) {
                Move m = encodeMove(4, 2);
                m = setCastle(m, true);
                moves.add(m);
            }
        }
    }
    else {
        if ((castlingRights & BLACKKSIDE)
         && (getOccupancy() & BLACK_KSIDE_PASSTHROUGH_SQS) == 0
         && !isInCheck(BLACK)) {
            if (getAttackMap(WHITE, 61) == 0) {
                Move m = encodeMove(60, 62);
                m = setCastle(m, true);
                moves.add(m);
            }
        }
        if ((castlingRights & BLACKQSIDE)
         && (getOccupancy() & BLACK_QSIDE_PASSTHROUGH_SQS) == 0
         && !isInCheck(BLACK)) {
            if (getAttackMap(WHITE, 59) == 0) {
                Move m = encodeMove(60, 58);
                m = setCastle(m, true);
                moves.add(m);
            }
        }
    }
}


//------------------------------------------------------------------------------
//-----------------------Useful bitboard info generators:-----------------------
//------------------------------attack maps, etc.-------------------------------
//------------------------------------------------------------------------------

// Get the attack map of all potential x-ray pieces (bishops, rooks, queens)
// after a blocker has been removed.
uint64_t Board::getXRayPieceMap(int color, int sq, int blockerColor,
    uint64_t blockerStart, uint64_t blockerEnd) {
    uint64_t occ = getOccupancy();
    occ ^= blockerStart;
    occ ^= blockerEnd;

    uint64_t bishops = pieces[color][BISHOPS];
    uint64_t rooks = pieces[color][ROOKS];
    uint64_t queens = pieces[color][QUEENS];

    uint64_t xRayMap = (getBishopSquares(sq, occ) & (bishops | queens))
                     | (getRookSquares(sq, occ) & (rooks | queens));

    return (xRayMap & ~blockerStart);
}

// Get the attack map of all potential x-ray pieces (bishops, rooks, queens)
// with no blockers removed. Used to compare with the results of the function
// above to find pins/discovered checks
/*uint64_t Board::getInitXRays(int color, int sq) {
    uint64_t bishops = pieces[color][BISHOPS];
    uint64_t rooks = pieces[color][ROOKS];
    uint64_t queens = pieces[color][QUEENS];

    return (getBishopSquares(sq) & (bishops | queens))
         | (getRookSquares(sq) & (rooks | queens));
}*/

// Given a color and a square, returns all pieces of the color that attack the
// square. Useful for checks, captures
uint64_t Board::getAttackMap(int color, int sq) {
    uint64_t occ = getOccupancy();
    uint64_t pawnCap = (color == WHITE)
                     ? getBPawnCaptures(INDEX_TO_BIT[sq])
                     : getWPawnCaptures(INDEX_TO_BIT[sq]);
    return (pawnCap & pieces[color][PAWNS])
         | (getKnightSquares(sq) & pieces[color][KNIGHTS])
         | (getBishopSquares(sq, occ) & (pieces[color][BISHOPS] | pieces[color][QUEENS]))
         | (getRookSquares(sq, occ) & (pieces[color][ROOKS] | pieces[color][QUEENS]))
         | (getKingSquares(sq) & pieces[color][KINGS]);
}

// Given the on a given square, used to get either the piece moving or the
// captured piece.
int Board::getPieceOnSquare(int color, int sq) {
    uint64_t endSingle = INDEX_TO_BIT[sq];
    for (int pieceID = 0; pieceID <= 5; pieceID++) {
        if (pieces[color][pieceID] & endSingle)
            return pieceID;
    }
    // If used for captures, the default of an empty square indicates an
    // en passant (and hopefully not an error).
    return -1;
}

// Returns true if a move puts the opponent in check
// Precondition: opposing king is not already in check (obviously)
// Does not consider en passant or castling
bool Board::isCheckMove(int color, Move m) {
    int kingSq = bitScanForward(pieces[color^1][KINGS]);

    // See if move is a direct check
    uint64_t attackMap = 0;
    uint64_t occ = getOccupancy();
    switch (getPieceOnSquare(color, getStartSq(m))) {
        case PAWNS:
            attackMap = (color == WHITE) 
                ? getBPawnCaptures(INDEX_TO_BIT[kingSq])
                : getWPawnCaptures(INDEX_TO_BIT[kingSq]);
            break;
        case KNIGHTS:
            attackMap = getKnightSquares(kingSq);
            break;
        case BISHOPS:
            attackMap = getBishopSquares(kingSq, occ);
            break;
        case ROOKS:
            attackMap = getRookSquares(kingSq, occ);
            break;
        case QUEENS:
            attackMap = getQueenSquares(kingSq, occ);
            break;
        case KINGS:
            // keep attackMap 0
            break;
    }
    if (INDEX_TO_BIT[getEndSq(m)] & attackMap)
        return true;

    // See if move is a discovered check
    // Get a bitboard of all pieces that could possibly xray
    uint64_t xrayPieces = pieces[color][BISHOPS] | pieces[color][ROOKS] | pieces[color][QUEENS];

    // Get any bishops, rooks, queens attacking king after piece has moved
    uint64_t xrays = getXRayPieceMap(color, kingSq, color, INDEX_TO_BIT[getStartSq(m)], INDEX_TO_BIT[getEndSq(m)]);
    // If there is an xray piece attacking the king square after the piece has
    // moved, we have discovered check
    if (xrays & xrayPieces)
        return true;

    // If not direct or discovered check, then not a check
    return false;
}

/*
 * These two functions return all squares x-rayed by a single rook or bishop, i.e.
 * all squares between the first and second blocker.
 * Algorithm from http://chessprogramming.wikispaces.com/X-ray+Attacks+%28Bitboards%29#ModifyingOccupancy
 */
uint64_t Board::getRookXRays(int sq, uint64_t occ, uint64_t blockers) {
    uint64_t attacks = getRookSquares(sq, occ);
    blockers &= attacks;
    return attacks ^ getRookSquares(sq, occ ^ blockers);
}

uint64_t Board::getBishopXRays(int sq, uint64_t occ, uint64_t blockers) {
    uint64_t attacks = getBishopSquares(sq, occ);
    blockers &= attacks;
    return attacks ^ getBishopSquares(sq, occ ^ blockers);
}

/*
 * This function returns all pinned pieces for a given side.
 * Algorithm from http://chessprogramming.wikispaces.com/Checks+and+Pinned+Pieces+%28Bitboards%29
 */
uint64_t Board::getPinnedMap(int color) {
    uint64_t pinned = 0;
    uint64_t blockers = allPieces[color];
    int kingSq = bitScanForward(pieces[color][KINGS]);

    uint64_t pinners = getRookXRays(kingSq, getOccupancy(), blockers)
        & (pieces[color^1][ROOKS] | pieces[color^1][QUEENS]);
    while (pinners) {
        int sq  = bitScanForward(pinners);
        pinned |= inBetweenSqs[sq][kingSq] & blockers;
        pinners &= pinners - 1;
    }

    pinners = getBishopXRays(kingSq, getOccupancy(), blockers)
        & (pieces[color^1][BISHOPS] | pieces[color^1][QUEENS]);
    while (pinners) {
        int sq  = bitScanForward(pinners);
        pinned |= inBetweenSqs[sq][kingSq] & blockers;
        pinners &= pinners - 1;
    }

    return pinned;
}


//------------------------------------------------------------------------------
//----------------------King: check, checkmate, stalemate-----------------------
//------------------------------------------------------------------------------

bool Board::isInCheck(int color) {
    int sq = bitScanForward(pieces[color][KINGS]);

    return getAttackMap(color^1, sq);
}

// Checks for mate: white is in check and has no legal moves
bool Board::isWInMate() {
    if (!isInCheck(WHITE))
        return false;

    MoveList moves = getAllLegalMoves(WHITE);
    return (moves.size() == 0);
}

// Checks for mate: black is in check and has no legal moves
bool Board::isBInMate() {
    if (!isInCheck(BLACK))
        return false;

    MoveList moves = getAllLegalMoves(BLACK);
    return (moves.size() == 0);
}

// Checks for stalemate: side to move is not in check, but has no legal moves
bool Board::isStalemate(int sideToMove) {
    if (isInCheck(sideToMove))
        return false;

    MoveList moves = getAllLegalMoves(sideToMove);
    return (moves.size() == 0);
}

bool Board::isDraw() {
    if (fiftyMoveCounter >= 100) return true;

    if (isInsufficientMaterial())
        return true;
    
    return false;
}

// Check for guaranteed drawn positions, where helpmate is not possible.
bool Board::isInsufficientMaterial() {
    int numPieces = count(allPieces[WHITE] | allPieces[BLACK]) - 2;
    if (numPieces < 2) {
        if (numPieces == 0)
            return true;
        if (pieces[WHITE][KNIGHTS] || pieces[WHITE][BISHOPS]
         || pieces[BLACK][KNIGHTS] || pieces[BLACK][BISHOPS])
            return true;
    }
    return false;
}


//------------------------------------------------------------------------------
//------------------------Evaluation and Move Ordering--------------------------
//------------------------------------------------------------------------------

/*
 * Evaluates the current board position in hundredths of pawns. White is
 * positive and black is negative in traditional negamax format.
 */
int Board::evaluate() {
    // Precompute some values
    int pieceCounts[2][6];
    for (int color = 0; color < 2; color++) {
        for (int pieceID = 0; pieceID < 6; pieceID++)
            pieceCounts[color][pieceID] = count(pieces[color][pieceID]);
    }

    // Material
    int whiteMaterial = PAWN_VALUE * pieceCounts[WHITE][PAWNS]
                    + KNIGHT_VALUE * pieceCounts[WHITE][KNIGHTS]
                    + BISHOP_VALUE * pieceCounts[WHITE][BISHOPS]
                    + ROOK_VALUE   * pieceCounts[WHITE][ROOKS]
                    + QUEEN_VALUE  * pieceCounts[WHITE][QUEENS];
    int blackMaterial = PAWN_VALUE * pieceCounts[BLACK][PAWNS]
                    + KNIGHT_VALUE * pieceCounts[BLACK][KNIGHTS]
                    + BISHOP_VALUE * pieceCounts[BLACK][BISHOPS]
                    + ROOK_VALUE   * pieceCounts[BLACK][ROOKS]
                    + QUEEN_VALUE  * pieceCounts[BLACK][QUEENS];

    // Compute endgame factor which is between 0 and EG_FACTOR_RES, inclusive
    int egFactor = EG_FACTOR_RES - (whiteMaterial + blackMaterial - START_VALUE / 2) * EG_FACTOR_RES / START_VALUE;
    egFactor = std::max(0, std::min(EG_FACTOR_RES, egFactor));


    // Check for special endgames
    if (egFactor == EG_FACTOR_RES) {
        int endgameScore = checkEndgameCases();
        if (endgameScore != -INFTY)
            return endgameScore;
    }


    //---------------------------Material terms---------------------------------
    int valueMg = 0;
    int valueEg = 0;

    valueMg += whiteMaterial;
    valueMg -= blackMaterial;
    valueEg += PAWN_VALUE_EG * pieceCounts[WHITE][PAWNS]
           + KNIGHT_VALUE_EG * pieceCounts[WHITE][KNIGHTS]
           + BISHOP_VALUE_EG * pieceCounts[WHITE][BISHOPS]
           + ROOK_VALUE_EG   * pieceCounts[WHITE][ROOKS]
           + QUEEN_VALUE_EG  * pieceCounts[WHITE][QUEENS];
    valueEg -= PAWN_VALUE_EG * pieceCounts[BLACK][PAWNS]
           + KNIGHT_VALUE_EG * pieceCounts[BLACK][KNIGHTS]
           + BISHOP_VALUE_EG * pieceCounts[BLACK][BISHOPS]
           + ROOK_VALUE_EG   * pieceCounts[BLACK][ROOKS]
           + QUEEN_VALUE_EG  * pieceCounts[BLACK][QUEENS];
    
    // Tempo bonus
    valueMg += (playerToMove == WHITE) ? TEMPO_VALUE : -TEMPO_VALUE;
    
    // Bishop pair bonus
    if ((pieces[WHITE][BISHOPS] & LIGHT) && (pieces[WHITE][BISHOPS] & DARK)) {
        valueMg += BISHOP_PAIR_VALUE;
        valueEg += BISHOP_PAIR_VALUE;
    }
    if ((pieces[BLACK][BISHOPS] & LIGHT) && (pieces[BLACK][BISHOPS] & DARK)) {
        valueMg -= BISHOP_PAIR_VALUE;
        valueEg -= BISHOP_PAIR_VALUE;
    }

    
    // Piece square tables
    // White pieces
    for (int pieceID = 0; pieceID < 6; pieceID++) {
        uint64_t bitboard = pieces[WHITE][pieceID];
        // Invert the board for white side
        bitboard = flipAcrossRanks(bitboard);
        while (bitboard) {
            int i = bitScanForward(bitboard);
            bitboard &= bitboard - 1;
            valueMg += midgamePieceValues[pieceID][i];
            valueEg += endgamePieceValues[pieceID][i];
        }
    }
    // Black pieces
    for (int pieceID = 0; pieceID < 6; pieceID++)  {
        uint64_t bitboard = pieces[BLACK][pieceID];
        while (bitboard) {
            int i = bitScanForward(bitboard);
            bitboard &= bitboard - 1;
            valueMg -= midgamePieceValues[pieceID][i];
            valueEg -= endgamePieceValues[pieceID][i];
        }
    }


    // Pawn value scaling: incur a penalty for having no pawns since the
    // ability to promote is gone
    const int PAWN_SCALING_MG[9] = {10, 3, 2, 2, 1, 0, 0, 0, 0};
    const int PAWN_SCALING_EG[9] = {71, 20, 8, 4, 2, 0, 0, 0, 0};

    valueMg -= PAWN_SCALING_MG[pieceCounts[WHITE][PAWNS]];
    valueEg -= PAWN_SCALING_EG[pieceCounts[WHITE][PAWNS]];
    valueMg += PAWN_SCALING_MG[pieceCounts[BLACK][PAWNS]];
    valueEg += PAWN_SCALING_EG[pieceCounts[BLACK][PAWNS]];

    // With queens on the board pawns are a target in the endgame
    if (pieces[WHITE][QUEENS])
        valueEg += 2 * pieceCounts[BLACK][PAWNS];
    if (pieces[BLACK][QUEENS])
        valueEg -= 2 * pieceCounts[WHITE][PAWNS];
    

    //----------------------------Positional terms------------------------------
    // A 32-bit integer that holds both the midgame and endgame values using SWAR
    Score value = EVAL_ZERO;
    
    
    //-----------------------King Safety and Mobility---------------------------
    // Scores based on mobility and basic king safety (which is turned off in
    // the endgame)
    PieceMoveList pmlWhite = getPieceMoveList<PML_PSEUDO_MOBILITY>(WHITE);
    PieceMoveList pmlBlack = getPieceMoveList<PML_PSEUDO_MOBILITY>(BLACK);
    getPseudoMobility(WHITE, pmlWhite, pmlBlack, valueMg, valueEg);
    getPseudoMobility(BLACK, pmlBlack, pmlWhite, valueMg, valueEg);


    // Consider squares near king
    int wKingSq = bitScanForward(pieces[WHITE][KINGS]);
    int bKingSq = bitScanForward(pieces[BLACK][KINGS]);
    uint64_t wKingNeighborhood = getKingSquares(wKingSq);
    uint64_t bKingNeighborhood = getKingSquares(bKingSq);

    // Calculate a separate king safety factor
    int whiteAttackers = KNIGHT_VALUE * pieceCounts[WHITE][KNIGHTS]
                       + BISHOP_VALUE * pieceCounts[WHITE][BISHOPS]
                       + ROOK_VALUE   * pieceCounts[WHITE][ROOKS]
                   + 2 * QUEEN_VALUE  * pieceCounts[WHITE][QUEENS];
    int blackAttackers = KNIGHT_VALUE * pieceCounts[BLACK][KNIGHTS]
                       + BISHOP_VALUE * pieceCounts[BLACK][BISHOPS]
                       + ROOK_VALUE   * pieceCounts[BLACK][ROOKS]
                   + 2 * QUEEN_VALUE  * pieceCounts[BLACK][QUEENS];
    const int FULL_KS_VALUE = 2 * KNIGHT_VALUE + 2 * BISHOP_VALUE + 2 * ROOK_VALUE + 2 * QUEEN_VALUE;

    int ksFactor = EG_FACTOR_RES - (whiteAttackers + blackAttackers - FULL_KS_VALUE / 2) * EG_FACTOR_RES / FULL_KS_VALUE;
    ksFactor = std::max(0, std::min(EG_FACTOR_RES, ksFactor));

    // All king safety terms are midgame only, so don't calculate them in the endgame
    if (ksFactor < EG_FACTOR_RES) {
        int ksValue = 0;

        ksValue += getKingSafety(pmlWhite, pmlBlack, wKingNeighborhood, bKingNeighborhood);

        // Castling rights
        ksValue += CASTLING_RIGHTS_VALUE[count(castlingRights & WHITECASTLE)];
        ksValue -= CASTLING_RIGHTS_VALUE[count(castlingRights & BLACKCASTLE)];
        
        // Pawn shield and storm values
        int wKingFile = wKingSq & 7;
        int bKingFile = bKingSq & 7;

        // White king
        for (int i = wKingFile-1; i <= wKingFile+1; i++) {
            if (i < 0 || i > 7)
                continue;

            uint64_t wPawnShield = pieces[WHITE][PAWNS] & FILES[i];
            if (wPawnShield) {
                int wPawnSq = bitScanForward(wPawnShield);
                int wr = wPawnSq >> 3;
                int wf = wPawnSq & 7;
                wf = std::min(wf, 7-wf);

                ksValue += PAWN_SHIELD_VALUE[wf][wr];
            }
            else
                ksValue += PAWN_SHIELD_VALUE[std::min(i, 7-i)][0];

            uint64_t bPawnStorm = pieces[BLACK][PAWNS] & FILES[i];
            if (bPawnStorm) {
                int bPawnSq = bitScanForward(bPawnStorm);
                int br = bPawnSq >> 3;
                int bf = bPawnSq & 7;
                bf = std::min(bf, 7-bf);
                uint64_t frontSqOcc = pieces[WHITE][PAWNS] & INDEX_TO_BIT[bPawnSq - 8];

                ksValue -= PAWN_STORM_VALUE[(frontSqOcc == 0)][bf][br];
            }
            else
                ksValue -= PAWN_STORM_VALUE[0][std::min(i, 7-i)][0];
        }

        // Black king
        for (int i = bKingFile-1; i <= bKingFile+1; i++) {
            if (i < 0 || i > 7)
                continue;

            uint64_t bPawnShield = pieces[BLACK][PAWNS] & FILES[i];
            if (bPawnShield) {
                int bPawnSq = bitScanReverse(bPawnShield);
                int br = 7 - (bPawnSq >> 3);
                int bf = bPawnSq & 7;
                bf = std::min(bf, 7-bf);

                ksValue -= PAWN_SHIELD_VALUE[bf][br];
            }
            else
                ksValue -= PAWN_SHIELD_VALUE[std::min(i, 7-i)][0];

            uint64_t wPawnStorm = pieces[WHITE][PAWNS] & FILES[i];
            if (wPawnStorm) {
                int wPawnSq = bitScanReverse(wPawnStorm);
                int wr = 7 - (wPawnSq >> 3);
                int wf = wPawnSq & 7;
                wf = std::min(wf, 7-wf);
                uint64_t frontSqOcc = pieces[BLACK][PAWNS] & INDEX_TO_BIT[wPawnSq - 8];

                ksValue += PAWN_STORM_VALUE[(frontSqOcc == 0)][wf][wr];
            }
            else
                ksValue += PAWN_STORM_VALUE[0][std::min(i, 7-i)][0];
        }

        valueMg += ksValue;
    }


    // Current pawn attacks
    // Used for outposts and backwards pawns
    uint64_t wPawnAtt = getWPawnCaptures(pieces[WHITE][PAWNS]);
    uint64_t bPawnAtt = getBPawnCaptures(pieces[BLACK][PAWNS]);
    // Get all squares attackable by pawns in the future
    // Used for outposts and backwards pawns
    uint64_t wPawnFrontSpan = pieces[WHITE][PAWNS] << 8;
    uint64_t bPawnFrontSpan = pieces[BLACK][PAWNS] >> 8;
    for (int i = 0; i < 5; i++) {
        wPawnFrontSpan |= wPawnFrontSpan << 8;
        bPawnFrontSpan |= bPawnFrontSpan >> 8;
    }
    uint64_t wPawnStopAtt = ((wPawnFrontSpan >> 1) & NOTH) | ((wPawnFrontSpan << 1) & NOTA);
    uint64_t bPawnStopAtt = ((bPawnFrontSpan >> 1) & NOTH) | ((bPawnFrontSpan << 1) & NOTA);


    //------------------------------Minor Pieces--------------------------------
    // Bishops tend to be worse if too many pawns are on squares of their color
    if (pieces[WHITE][BISHOPS] & LIGHT) {
        value -= BISHOP_PAWN_COLOR_PENALTY * count(pieces[WHITE][PAWNS] & LIGHT);
    }
    if (pieces[WHITE][BISHOPS] & DARK) {
        value -= BISHOP_PAWN_COLOR_PENALTY * count(pieces[WHITE][PAWNS] & DARK);
    }
    if (pieces[BLACK][BISHOPS] & LIGHT) {
        value += BISHOP_PAWN_COLOR_PENALTY * count(pieces[BLACK][PAWNS] & LIGHT);
    }
    if (pieces[BLACK][BISHOPS] & DARK) {
        value += BISHOP_PAWN_COLOR_PENALTY * count(pieces[BLACK][PAWNS] & DARK);
    }

    // Knights do better when the opponent has many pawns
    value += KNIGHT_PAWN_BONUS * pieceCounts[WHITE][KNIGHTS] * pieceCounts[BLACK][PAWNS];
    value -= KNIGHT_PAWN_BONUS * pieceCounts[BLACK][KNIGHTS] * pieceCounts[WHITE][PAWNS];

    // Knight outposts: knights that cannot be attacked by opposing pawns
    const uint64_t RANKS_456 = RANKS[3] | RANKS[4] | RANKS[5];
    const uint64_t RANKS_543 = RANKS[4] | RANKS[3] | RANKS[2];
    const uint64_t FILES_CDEF = FILES[2] | FILES[3] | FILES[4] | FILES[5];
    if (uint64_t wOutpost = pieces[WHITE][KNIGHTS] & ~bPawnStopAtt & RANKS_456 & FILES_CDEF) {
        value += KNIGHT_OUTPOST_BONUS * count(wOutpost);
        // An additional bonus if the outpost knight is defended by a pawn
        value += KNIGHT_OUTPOST_PAWN_DEF_BONUS * count(wOutpost & wPawnAtt);
    }
    if (uint64_t bOutpost = pieces[BLACK][KNIGHTS] & ~wPawnStopAtt & RANKS_543 & FILES_CDEF) {
        value -= KNIGHT_OUTPOST_BONUS * count(bOutpost);
        value -= KNIGHT_OUTPOST_PAWN_DEF_BONUS * count(bOutpost & bPawnAtt);
    }

    // Bishop outposts
    if (uint64_t wOutpost = pieces[WHITE][BISHOPS] & ~bPawnStopAtt & RANKS_456 & FILES_CDEF) {
        value += BISHOP_OUTPOST_BONUS * count(wOutpost);
        value += BISHOP_OUTPOST_PAWN_DEF_BONUS * count(wOutpost & wPawnAtt);
    }
    if (uint64_t bOutpost = pieces[BLACK][BISHOPS] & ~wPawnStopAtt & RANKS_543 & FILES_CDEF) {
        value -= BISHOP_OUTPOST_BONUS * count(bOutpost);
        value -= BISHOP_OUTPOST_PAWN_DEF_BONUS * count(bOutpost & bPawnAtt);
    }

    // Special case: Nc3 blocking c2-c4 in closed openings (pawn on d4, no pawn on e4)
    if ((pieces[WHITE][KNIGHTS] & 0x40000)
     && (pieces[WHITE][PAWNS] & 0x400)
     && (pieces[WHITE][PAWNS] & 0x8000000)
    && !(pieces[WHITE][PAWNS] & 0x10000000))
        value -= KNIGHT_C3_CLOSED_PENALTY;
    if ((pieces[BLACK][KNIGHTS] & 0x40000000000)
     && (pieces[BLACK][PAWNS] & 0x4000000000000)
     && (pieces[BLACK][PAWNS] & 0x800000000)
    && !(pieces[BLACK][PAWNS] & 0x1000000000))
        value += KNIGHT_C3_CLOSED_PENALTY;


    //-------------------------------Rooks--------------------------------------
    // Bonus for having rooks on open files
    uint64_t wRooksOpen = pieces[WHITE][ROOKS];
    while (wRooksOpen) {
        int rookSq = bitScanForward(wRooksOpen);
        wRooksOpen &= wRooksOpen - 1;
        int file = rookSq & 7;

        if (!(FILES[file] & (pieces[WHITE][PAWNS] | pieces[BLACK][PAWNS])))
            value += ROOK_OPEN_FILE_BONUS;
    }

    uint64_t bRooksOpen = pieces[BLACK][ROOKS];
    while (bRooksOpen) {
        int rookSq = bitScanForward(bRooksOpen);
        bRooksOpen &= bRooksOpen - 1;
        int file = rookSq & 7;

        if (!(FILES[file] & (pieces[WHITE][PAWNS] | pieces[BLACK][PAWNS])))
            value -= ROOK_OPEN_FILE_BONUS;
    }

    
    //----------------------------Pawn structure--------------------------------
    // Passed pawns
    uint64_t wPassedBlocker = pieces[BLACK][PAWNS] >> 8;
    uint64_t bPassedBlocker = pieces[WHITE][PAWNS] << 8;
    // If opposing pawns are on the same or an adjacent file on a pawn's front
    // span, then the pawn is not passed
    wPassedBlocker |= ((wPassedBlocker >> 1) & NOTH) | ((wPassedBlocker << 1) & NOTA);
    bPassedBlocker |= ((bPassedBlocker >> 1) & NOTH) | ((bPassedBlocker << 1) & NOTA);
    // Include own pawns as blockers to prevent doubled pawns from both being
    // scored as passers
    wPassedBlocker |= (pieces[WHITE][PAWNS] >> 8);
    bPassedBlocker |= (pieces[BLACK][PAWNS] << 8);
    // Find opposing pawn front spans
    for(int i = 0; i < 4; i++) {
        wPassedBlocker |= (wPassedBlocker >> 8);
        bPassedBlocker |= (bPassedBlocker << 8);
    }
    // Passers are pawns outside the opposing pawn front span
    uint64_t wPassedPawns = pieces[WHITE][PAWNS] & ~wPassedBlocker;
    uint64_t bPassedPawns = pieces[BLACK][PAWNS] & ~bPassedBlocker;

    // Give penalties if the passed pawn is blockaded by a knight, bishop, or king
    uint64_t whiteBlockaders = pieces[BLACK][KNIGHTS] | pieces[BLACK][BISHOPS] | pieces[BLACK][KINGS];
    uint64_t blackBlockaders = pieces[WHITE][KNIGHTS] | pieces[WHITE][BISHOPS] | pieces[WHITE][KINGS];
    uint64_t wPasserTemp = wPassedPawns;
    while (wPasserTemp) {
        int passerSq = bitScanForward(wPasserTemp);
        wPasserTemp &= wPasserTemp - 1;
        int file = passerSq & 7;
        int rank = passerSq >> 3;
        value += PASSER_BONUS[rank];
        value += PASSER_FILE_BONUS[file];
        if ((INDEX_TO_BIT[passerSq] << 8) & whiteBlockaders)
            value -= BLOCKADED_PASSER_PENALTY;
    }
    uint64_t bPasserTemp = bPassedPawns;
    while (bPasserTemp) {
        int passerSq = bitScanForward(bPasserTemp);
        bPasserTemp &= bPasserTemp - 1;
        int file = passerSq & 7;
        int rank = 7 - (passerSq >> 3);
        value -= PASSER_BONUS[rank];
        value -= PASSER_FILE_BONUS[file];
        if ((INDEX_TO_BIT[passerSq] >> 8) & blackBlockaders)
            value += BLOCKADED_PASSER_PENALTY;
    }
    
    int wPawnCtByFile[8];
    int bPawnCtByFile[8];
    for (int i = 0; i < 8; i++) {
        wPawnCtByFile[i] = count(pieces[WHITE][PAWNS] & FILES[i]);
        bPawnCtByFile[i] = count(pieces[BLACK][PAWNS] & FILES[i]);
    }
    
    // Doubled pawns
    for (int i = 0; i < 8; i++) {
        value -= DOUBLED_PENALTY[wPawnCtByFile[i]] * DOUBLED_PENALTY_SCALE[pieceCounts[WHITE][PAWNS]];
        value += DOUBLED_PENALTY[bPawnCtByFile[i]] * DOUBLED_PENALTY_SCALE[pieceCounts[BLACK][PAWNS]];
    }
    
    // Isolated pawns
    // Fill a bitmap of which files have pawns
    uint64_t wp = 0, bp = 0;
    for (int i = 0; i < 8; i++) {
        wp |= (bool) (wPawnCtByFile[i]);
        bp |= (bool) (bPawnCtByFile[i]);
        wp <<= 1;
        bp <<= 1;
    }
    wp >>= 1;
    bp >>= 1;
    // If there are pawns on either adjacent file, we remove this pawn
    wp &= ~((wp >> 1) | (wp << 1));
    bp &= ~((bp >> 1) | (bp << 1));
    int whiteIsolated = count(wp);
    int blackIsolated = count(bp);
    value -= ISOLATED_PENALTY * whiteIsolated;
    value += ISOLATED_PENALTY * blackIsolated;
    value -= CENTRAL_ISOLATED_PENALTY * count(wp & 0x7E);
    value += CENTRAL_ISOLATED_PENALTY * count(bp & 0x7E);

    // Isolated, doubled pawns
    // x1 for isolated, doubled pawns
    // x3 for isolated, tripled pawns
    for (int i = 0; i < 8; i++) {
        if ((wPawnCtByFile[i] > 1) && (wp & INDEX_TO_BIT[7-i])) {
            value -= ISOLATED_DOUBLED_PENALTY * ((wPawnCtByFile[i] - 1) * wPawnCtByFile[i]) / 2;
        }
        if ((bPawnCtByFile[i] > 1) && (bp & INDEX_TO_BIT[7-i])) {
            value += ISOLATED_DOUBLED_PENALTY * ((bPawnCtByFile[i] - 1) * bPawnCtByFile[i]) / 2;
        }
    }

    // Backward pawns
    uint64_t wBadStopSqs = ~wPawnStopAtt & bPawnAtt;
    uint64_t bBadStopSqs = ~bPawnStopAtt & wPawnAtt;
    for (int i = 0; i < 6; i++) {
        wBadStopSqs |= wBadStopSqs >> 8;
        bBadStopSqs |= bBadStopSqs << 8;
    }

    uint64_t wBackwards = wBadStopSqs & pieces[WHITE][PAWNS];
    uint64_t bBackwards = bBadStopSqs & pieces[BLACK][PAWNS];
    value -= BACKWARD_PENALTY * count(wBackwards);
    value += BACKWARD_PENALTY * count(bBackwards);


    // King-pawn tropism
    int kingPawnTropism = 0;
    if (egFactor > 0) {
        uint64_t pawnBits = pieces[WHITE][PAWNS] | pieces[BLACK][PAWNS];
        int pawnCount = pieceCounts[WHITE][PAWNS] + pieceCounts[BLACK][PAWNS];

        int wTropismTotal = 0, bTropismTotal = 0;
        while (pawnBits) {
            int pawnSq = bitScanForward(pawnBits);
            int rank = pawnSq >> 3;
            pawnBits &= pawnBits - 1;
            if (INDEX_TO_BIT[pawnSq] & wPassedPawns) {
                wTropismTotal += 4 * getManhattanDistance(pawnSq, wKingSq) * rank;
                bTropismTotal += 7 * getManhattanDistance(pawnSq, bKingSq) * rank;
            }
            else if (INDEX_TO_BIT[pawnSq] & bPassedPawns) {
                wTropismTotal += 7 * getManhattanDistance(pawnSq, wKingSq) * (7-rank);
                bTropismTotal += 4 * getManhattanDistance(pawnSq, bKingSq) * (7-rank);
            }
            else if (INDEX_TO_BIT[pawnSq] & (wBackwards | bBackwards)) {
                wTropismTotal += 2 * getManhattanDistance(pawnSq, wKingSq);
                bTropismTotal += 2 * getManhattanDistance(pawnSq, bKingSq);
            }
            else {
                wTropismTotal += getManhattanDistance(pawnSq, wKingSq);
                bTropismTotal += getManhattanDistance(pawnSq, bKingSq);
            }
        }

        if (pawnCount)
            kingPawnTropism = (bTropismTotal - wTropismTotal) / pawnCount;

        valueEg += kingPawnTropism;
    }


    valueMg += decEvalMg(value);
    valueEg += decEvalEg(value);
    int totalEval = (valueMg * (EG_FACTOR_RES - egFactor) + valueEg * egFactor) / EG_FACTOR_RES;

    // Scale factors
    // Opposite colored bishops
    if (egFactor > 3 * EG_FACTOR_RES / 4) {
        if (pieceCounts[WHITE][BISHOPS] == 1
         && pieceCounts[BLACK][BISHOPS] == 1
         && (((pieces[WHITE][BISHOPS] & LIGHT) && (pieces[BLACK][BISHOPS] & DARK))
          || ((pieces[WHITE][BISHOPS] & DARK) && (pieces[BLACK][BISHOPS] & LIGHT)))) {
            if ((getNonPawnMaterial(WHITE) == pieces[WHITE][BISHOPS])
             && (getNonPawnMaterial(BLACK) == pieces[BLACK][BISHOPS]))
                totalEval = totalEval / 2;
            else
                totalEval = (384 - 128 * egFactor / EG_FACTOR_RES) * totalEval / 256;
        }
    }

    
    return totalEval;
}

/* Scores the board for a player based on estimates of mobility
 * This function also provides a bonus for having mobile pieces near the
 * opponent's king, and deals with control of center.
 */
void Board::getPseudoMobility(int color, PieceMoveList &pml, PieceMoveList &oppPml,
    int &valueMg, int &valueEg) {
    // Bitboard of center 4 squares: d4, e4, d5, e5
    const uint64_t CENTER_SQS = 0x0000001818000000;
    // Value of each square in the extended center in cp
    const int EXTENDED_CENTER_VAL = 2;
    // Additional bonus for squares in the center four squares in cp, in addition
    // to EXTENDED_CENTER_VAL
    const int CENTER_BONUS = 2;
    // Holds the mobility values and the final result
    int mgMobility = 0, egMobility = 0;
    // Holds the center control score
    int centerControl = 0;
    // All squares the side to move could possibly move to
    uint64_t openSqs = ~allPieces[color];
    // Extended center: center, plus c4, f4, c5, f5, and d6/d3, e6/e3
    uint64_t EXTENDED_CENTER_SQS = (color == WHITE) ? 0x0000183C3C000000
                                                    : 0x0000003C3C180000;

    // Calculate center control only for pawns
    uint64_t pawns = pieces[color][PAWNS];
    uint64_t pawnAttackMap = (color == WHITE) ? getWPawnCaptures(pawns)
                                              : getBPawnCaptures(pawns);
    centerControl += EXTENDED_CENTER_VAL * count(pawnAttackMap & EXTENDED_CENTER_SQS);
    centerControl += CENTER_BONUS * count(pawnAttackMap & CENTER_SQS);


    uint64_t oppPawns = pieces[color^1][PAWNS];
    uint64_t oppPawnAttackMap = (color == WHITE) ? getBPawnCaptures(oppPawns)
                                                 : getWPawnCaptures(oppPawns);
    // We count mobility for captures or moves to open squares not controlled
    // by an opponent's pawn
    openSqs = allPieces[color^1] | (openSqs & ~oppPawnAttackMap);
    // Or for a queen, squares not controlled by an opponent's pawn or minor
    uint64_t oppAttackMap = 0;
    for (unsigned int i = 0; i < oppPml.size(); i++) {
        PieceMoveInfo pmi = oppPml.get(i);
        if (pmi.pieceID <= BISHOPS)
            oppAttackMap |= pmi.legal;
    }

    // Iterate over piece move information to extract all mobility-related scores
    for (unsigned int i = 0; i < pml.size(); i++) {
        PieceMoveInfo pmi = pml.get(i);

        int pieceIndex = pmi.pieceID - 1;
        // Get all potential legal moves
        uint64_t legal = pmi.legal;
        // Get mobility score
        if (pieceIndex == QUEENS - 1) {
            mgMobility += mobilityScore[0][pieceIndex][count(legal & openSqs & ~oppAttackMap)];
            egMobility += mobilityScore[1][pieceIndex][count(legal & openSqs & ~oppAttackMap)];
        }
        else {
            mgMobility += mobilityScore[0][pieceIndex][count(legal & openSqs)];
            egMobility += mobilityScore[1][pieceIndex][count(legal & openSqs)];
        }

        // Get center control score
        if (pieceIndex == QUEENS - 1) {
            centerControl += count(legal & EXTENDED_CENTER_SQS);
            centerControl += count(legal & CENTER_SQS);
        }
        else {
            centerControl += EXTENDED_CENTER_VAL * count(legal & EXTENDED_CENTER_SQS);
            centerControl += CENTER_BONUS * count(legal & CENTER_SQS);
        }
    }


    if (color == WHITE) {
        valueMg += mgMobility + centerControl;
        valueEg += egMobility;
    }
    else {
        valueMg -= mgMobility + centerControl;
        valueEg -= egMobility;
    }
}

// King safety, based on the number of opponent pieces near the king
// The lookup table approach is inspired by Ed Schroder's Rebel chess engine
int Board::getKingSafety(PieceMoveList &pmlWhite, PieceMoveList &pmlBlack,
    uint64_t wKingSqs, uint64_t bKingSqs) {
    // Scale factor for pieces attacking opposing king
    const int KING_THREAT_MULTIPLIER[4] = {3, 3, 4, 6};
    // const int KING_DEFENSE_MULTIPLIER[4] = {2, 2, 1, 0};

    // Holds the king safety score
    int wKingSafety = 0, bKingSafety = 0;
    // Counts the number of pieces participating in the king attack
    int wKingAttackPieces = 0, bKingAttackPieces = 0;

    uint64_t wKingNeighborhood = wKingSqs | (wKingSqs << 8);
    uint64_t bKingNeighborhood = bKingSqs | (bKingSqs >> 8);


    // Analyze undefended squares directly adjacent to king
    uint64_t wKingDefenseless = 0, bKingDefenseless = 0;
    for (unsigned int i = 0; i < pmlWhite.size(); i++) {
        PieceMoveInfo pmi = pmlWhite.get(i);
        uint64_t legal = pmi.legal;

        wKingDefenseless |= legal & wKingSqs;
    }
    // Add in pawns
    wKingDefenseless |= getWPawnCaptures(pieces[WHITE][PAWNS]) & wKingSqs;
    wKingDefenseless ^= wKingSqs;

    for (unsigned int i = 0; i < pmlBlack.size(); i++) {
        PieceMoveInfo pmi = pmlBlack.get(i);
        uint64_t legal = pmi.legal;

        bKingDefenseless |= legal & bKingSqs;
    }
    bKingDefenseless |= getBPawnCaptures(pieces[BLACK][PAWNS]) & bKingSqs;
    bKingDefenseless ^= bKingSqs;

    // wKingSafety += 4 * count(bKingDefenseless);
    // bKingSafety += 4 * count(wKingDefenseless);


    // Iterate over piece move information to extract all mobility-related scores
    for (unsigned int i = 0; i < pmlWhite.size(); i++) {
        PieceMoveInfo pmi = pmlWhite.get(i);

        int pieceIndex = pmi.pieceID - 1;
        // Get all potential legal moves
        uint64_t legal = pmi.legal;

        // Get king safety score
        int kingSqCount = count(legal & bKingNeighborhood);
        if (kingSqCount) {
            wKingAttackPieces++;
            // wKingSafety += KING_THREAT_MULTIPLIER[pieceIndex]
            //              + KING_OUTER_THREAT_MULTIPLIER[pieceIndex];
            wKingSafety += KING_THREAT_MULTIPLIER[pieceIndex] * count(legal & bKingSqs);

            // Bonus for overloading on defenseless squares
            wKingSafety += 5 * count(legal & bKingDefenseless);
        }

        // if (legal & wKingNeighborhood)
        //     wKingSafety -= KING_DEFENSE_MULTIPLIER[pieceIndex];
    }

    for (unsigned int i = 0; i < pmlBlack.size(); i++) {
        PieceMoveInfo pmi = pmlBlack.get(i);

        int pieceIndex = pmi.pieceID - 1;
        uint64_t legal = pmi.legal;

        int kingSqCount = count(legal & wKingNeighborhood);
        if (kingSqCount) {
            bKingAttackPieces++;
            // bKingSafety += KING_THREAT_MULTIPLIER[pieceIndex]
            //              + KING_OUTER_THREAT_MULTIPLIER[pieceIndex];
            bKingSafety += KING_THREAT_MULTIPLIER[pieceIndex] * count(legal & wKingSqs);

            bKingSafety += 5 * count(legal & wKingDefenseless);
        }

        // if (legal & bKingNeighborhood)
        //     bKingSafety -= KING_DEFENSE_MULTIPLIER[pieceIndex];
    }

    // Give a decent bonus for each additional piece participating
    wKingSafety += std::min(20, wKingAttackPieces * (wKingAttackPieces-1) + 6);
    bKingSafety += std::min(20, bKingAttackPieces * (wKingAttackPieces-1) + 6);

    // If at least two pieces are involved in the attack, we consider it "serious"
    if (wKingAttackPieces >= 3) {

    }
    else if (wKingAttackPieces == 2)
        wKingSafety = 3 * wKingSafety / 4;
    else
        wKingSafety = 0;
    if (bKingAttackPieces >= 3) {

    }
    else if (bKingAttackPieces == 2)
        bKingSafety = 3 * bKingSafety / 4;
    else
        bKingSafety = 0;

    // Reduce attack slightly if there is a good pawn shield
    bKingSafety -= count(wKingNeighborhood & pieces[WHITE][PAWNS] & 0xe7e7e7e7e7e7e7e7);
    wKingSafety -= count(bKingNeighborhood & pieces[BLACK][PAWNS] & 0xe7e7e7e7e7e7e7e7);
    // Extra for pawns directly in front of the king
    bKingSafety -= count(wKingSqs & pieces[WHITE][PAWNS] & 0xe7e7e7e7e7e7e7e7);
    wKingSafety -= count(bKingSqs & pieces[BLACK][PAWNS] & 0xe7e7e7e7e7e7e7e7);

    const int KS_TO_SCORE[100] = {
          0,   0,   0,   1,   1,   2,   3,   4,   6,   8,
         11,  14,  17,  20,  24,  27,  31,  34,  38,  42,
         46,  50,  54,  58,  62,  66,  70,  74,  78,  83, 
         87,  92,  96, 101, 105, 110, 115, 120, 125, 130,
        135, 140, 145, 150, 155, 160, 165, 170, 175, 180,
        185, 190, 195, 200, 205, 210, 215, 220, 225, 230,
        235, 240, 245, 250, 255, 260, 265, 270, 275, 280,
        285, 290, 295, 300, 305, 310, 315, 320, 325, 330,
        335, 340, 345, 350, 355, 360, 365, 370, 375, 380,
        385, 390, 395, 400, 405, 410, 415, 420, 425, 430
    };

    return (KS_TO_SCORE[std::max(0, std::min(99, wKingSafety))]
          - KS_TO_SCORE[std::max(0, std::min(99, bKingSafety))]);
}

// Check special endgame cases: where help mate is possible (detecting this
// is delegated to search), but forced mate is not, or where a simple
// forced mate is possible.
int Board::checkEndgameCases() {
    int numWPieces = count(allPieces[WHITE]) - 1;
    int numBPieces = count(allPieces[BLACK]) - 1;
    int numPieces = numWPieces + numBPieces;

    // Rook or queen + anything else vs. lone king is a forced win
    if (numBPieces == 0 && (pieces[WHITE][ROOKS] || pieces[WHITE][QUEENS])) {
        return scoreSimpleKnownWin(WHITE);
    }
    if (numWPieces == 0 && (pieces[BLACK][ROOKS] || pieces[BLACK][QUEENS])) {
        return scoreSimpleKnownWin(BLACK);
    }

    if (numPieces == 1) {
        if (pieces[WHITE][PAWNS]) {
            int wPawn = bitScanForward(flipAcrossRanks(pieces[WHITE][PAWNS]));
            return 3 * PAWN_VALUE_EG / 2 + endgamePieceValues[PAWNS][wPawn];
        }
        if (pieces[BLACK][PAWNS]) {
            int bPawn = bitScanForward(pieces[BLACK][PAWNS]);
            return -3 * PAWN_VALUE_EG / 2 - endgamePieceValues[PAWNS][bPawn];
        }
    }

    else if (numPieces == 2) {
        // If white has one piece, the other must be black's
        if (numWPieces == 1) {
            // If each side has one minor piece, then draw
            if ((pieces[WHITE][KNIGHTS] | pieces[WHITE][BISHOPS])
             && (pieces[BLACK][KNIGHTS] | pieces[BLACK][BISHOPS]))
                return 0;
            // If each side has a rook, then draw
            if (pieces[WHITE][ROOKS] && pieces[BLACK][ROOKS])
                return 0;
            // If each side has a queen, then draw
            if (pieces[WHITE][QUEENS] && pieces[BLACK][QUEENS])
                return 0;
        }
        // Otherwise, one side has both pieces
        else {
            // Pawn + anything is a win
            // TODO Not always true, also needs code simplification
            if (pieces[WHITE][PAWNS]) {
                int value = KNOWN_WIN / 2;
                // White pieces
                for (int pieceID = 0; pieceID < 6; pieceID++) {
                    uint64_t bitboard = pieces[0][pieceID];
                    // Invert the board for white side
                    bitboard = flipAcrossRanks(bitboard);
                    while (bitboard) {
                        int i = bitScanForward(bitboard);
                        bitboard &= bitboard - 1;
                        value += endgamePieceValues[pieceID][i];
                    }
                }
                int bKing = bitScanForward(pieces[BLACK][KINGS]);
                value -= endgamePieceValues[KINGS][bKing];
                return value;
            }
            if (pieces[BLACK][PAWNS]) {
                int value = -KNOWN_WIN / 2;
                // Black pieces
                for (int pieceID = 0; pieceID < 6; pieceID++)  {
                    uint64_t bitboard = pieces[1][pieceID];
                    while (bitboard) {
                        int i = bitScanForward(bitboard);
                        bitboard &= bitboard - 1;
                        value -= endgamePieceValues[pieceID][i];
                    }
                }
                int wKing = bitScanForward(pieces[WHITE][KINGS]);
                value += endgamePieceValues[KINGS][wKing];
                return value;
            }
            // Two knights is a draw
            if (count(pieces[WHITE][KNIGHTS]) == 2 || count(pieces[BLACK][KNIGHTS]) == 2)
                return 0;
            // Two bishops is a win
            if (count(pieces[WHITE][BISHOPS]) == 2)
                return scoreSimpleKnownWin(WHITE);
            if (count(pieces[BLACK][BISHOPS]) == 2)
                return scoreSimpleKnownWin(BLACK);

            // Mating with knight and bishop
            if (pieces[WHITE][KNIGHTS] && pieces[WHITE][BISHOPS]) {
                int value = KNOWN_WIN;
                int wKing = bitScanForward(pieces[WHITE][KINGS]);
                int bKing = bitScanForward(pieces[BLACK][KINGS]);
                value += endgamePieceValues[KINGS][wKing] - endgamePieceValues[KINGS][bKing];

                // Light squared corners are H1 (7) and A8 (56)
                if (pieces[WHITE][BISHOPS] & LIGHT) {
                    value -= 20 * std::min(getManhattanDistance(bKing, 7), getManhattanDistance(bKing, 56));
                }
                // Dark squared corners are A1 (0) and H8 (63)
                else {
                    value -= 20 * std::min(getManhattanDistance(bKing, 0), getManhattanDistance(bKing, 63));
                }
                return value;
            }
            if (pieces[BLACK][KNIGHTS] && pieces[BLACK][BISHOPS]) {
                int value = -KNOWN_WIN;
                int wKing = bitScanForward(pieces[WHITE][KINGS]);
                int bKing = bitScanForward(pieces[BLACK][KINGS]);
                value += endgamePieceValues[KINGS][wKing] - endgamePieceValues[KINGS][bKing];

                // Light squared corners are H1 (7) and A8 (56)
                if (pieces[BLACK][BISHOPS] & LIGHT) {
                    value += 20 * std::min(getManhattanDistance(wKing, 7), getManhattanDistance(wKing, 56));
                }
                // Dark squared corners are A1 (0) and H8 (63)
                else {
                    value += 20 * std::min(getManhattanDistance(wKing, 0), getManhattanDistance(wKing, 63));
                }
                return value;
            }
        }
    }

    // If not an endgame case, return -INFTY
    return -INFTY;
}

// A function for scoring the most basic mating cases, when it is only necessary
// to get the opposing king into a corner.
int Board::scoreSimpleKnownWin(int winningColor) {
    int wKing = bitScanForward(pieces[WHITE][KINGS]);
    int bKing = bitScanForward(pieces[BLACK][KINGS]);
    int winScore = (winningColor == WHITE) ? KNOWN_WIN : -KNOWN_WIN;
    return winScore + endgamePieceValues[KINGS][wKing]
                    - endgamePieceValues[KINGS][bKing];
}

// Gets the endgame factor, which adjusts the evaluation based on how much
// material is left on the board.
int Board::getEGFactor() {
    int whiteMaterial = getMaterial(WHITE);
    int blackMaterial = getMaterial(BLACK);
    int egFactor = EG_FACTOR_RES - (whiteMaterial + blackMaterial - START_VALUE / 2) * EG_FACTOR_RES / START_VALUE;
    return std::max(0, std::min(EG_FACTOR_RES, egFactor));
}

int Board::getMaterial(int color) {
    return PAWN_VALUE   * count(pieces[color][PAWNS])
         + KNIGHT_VALUE * count(pieces[color][KNIGHTS])
         + BISHOP_VALUE * count(pieces[color][BISHOPS])
         + ROOK_VALUE   * count(pieces[color][ROOKS])
         + QUEEN_VALUE  * count(pieces[color][QUEENS]);
}

uint64_t Board::getNonPawnMaterial(int color) {
    return pieces[color][KNIGHTS] | pieces[color][BISHOPS]
         | pieces[color][ROOKS]   | pieces[color][QUEENS];
}

int Board::getManhattanDistance(int sq1, int sq2) {
    return std::abs((sq1 >> 3) - (sq2 >> 3)) + std::abs((sq1 & 7) - (sq2 & 7));
}

// Given a bitboard of attackers, finds the least valuable attacker of color and
// returns a single occupancy bitboard of that piece
uint64_t Board::getLeastValuableAttacker(uint64_t attackers, int color, int &piece) {
    for (piece = 0; piece < 5; piece++) {
        uint64_t single = attackers & pieces[color][piece];
        if (single)
            return single & -single;
    }

    piece = KINGS;
    return attackers & pieces[color][KINGS];
}

// Static exchange evaluation algorithm from
// https://chessprogramming.wikispaces.com/SEE+-+The+Swap+Algorithm
int Board::getSEE(int color, int sq) {
    int gain[32], d = 0, piece = 0;
    uint64_t attackers = getAttackMap(WHITE, sq) | getAttackMap(BLACK, sq);
    // used attackers that may act as blockers for x-ray pieces
    uint64_t used = 0;

    // Get the first attacker
    uint64_t single = getLeastValuableAttacker(attackers, color, piece);
    // If there are no attackers, return immediately
    if (!single)
        return 0;
    // Get value of piece initially being captured, or 0 if the square is
    // empty
    gain[d] = SEE_PIECE_VALS[getPieceOnSquare(color^1, sq)];

    do {
        d++; // next depth
        color ^= 1;
        gain[d]  = SEE_PIECE_VALS[piece] - gain[d-1];
        if (-gain[d-1] < 0 && gain[d] < 0) // pruning for stand pat
            break;
        attackers ^= single; // remove used attacker
        used |= single;
        attackers |= getXRayPieceMap(WHITE, sq, color, used, 0) | getXRayPieceMap(BLACK, sq, color, used, 0);
        single = getLeastValuableAttacker(attackers, color, piece);
    } while (single);

    // Minimax results
    while (--d)
        gain[d-1]= -((-gain[d-1] > gain[d]) ? -gain[d-1] : gain[d]);

    return gain[0];
}

/**
 * @brief This function calculates a SEE for a move rather than a square, by
 * forcing the initial move (to avoid standing pat if the move is bad).
 */
int Board::getSEEForMove(int color, Move m) {
    int value = 0;
    int startSq = getStartSq(m);
    int endSq = getEndSq(m);
    int pieceID = getPieceOnSquare(color, startSq);

    // TODO temporary hack for EP captures
    if (isEP(m))
        return -PAWN_VALUE;

    // Do the move temporarily
    pieces[color][pieceID] &= ~INDEX_TO_BIT[startSq];
    pieces[color][pieceID] |= INDEX_TO_BIT[endSq];
    allPieces[color] &= ~INDEX_TO_BIT[startSq];
    allPieces[color] |= INDEX_TO_BIT[endSq];

    // For a capture, we also need to update the captured piece on the board
    if (isCapture(m)) {
        int capturedPiece = getPieceOnSquare(color^1, endSq);
        pieces[color^1][capturedPiece] &= ~INDEX_TO_BIT[endSq];
        allPieces[color^1] &= ~INDEX_TO_BIT[endSq];

        value = SEE_PIECE_VALS[capturedPiece] - getSEE(color^1, endSq);

        pieces[color^1][capturedPiece] |= INDEX_TO_BIT[endSq];
        allPieces[color^1] |= INDEX_TO_BIT[endSq];
    }
    else {
        value = -getSEE(color^1, endSq);
    }

    // Undo the move
    pieces[color][pieceID] |= INDEX_TO_BIT[startSq];
    pieces[color][pieceID] &= ~INDEX_TO_BIT[endSq];
    allPieces[color] |= INDEX_TO_BIT[startSq];
    allPieces[color] &= ~INDEX_TO_BIT[endSq];

    return value;
}

int Board::valueOfPiece(int piece) {
    switch(piece) {
        case -1:
            return 0;
        case PAWNS:
            return PAWN_VALUE;
        case KNIGHTS:
            return KNIGHT_VALUE;
        case BISHOPS:
            return BISHOP_VALUE;
        case ROOKS:
            return ROOK_VALUE;
        case QUEENS:
            return QUEEN_VALUE;
        case KINGS:
            return MATE_SCORE;
    }

    return -1;
}

// Calculates a score for Most Valuable Victim / Least Valuable Attacker
// capture ordering.
int Board::getMVVLVAScore(int color, Move m) {
    int endSq = getEndSq(m);
    int attacker = getPieceOnSquare(color, getStartSq(m));
    if (attacker == KINGS)
        attacker = -1;
    int victim = getPieceOnSquare(color^1, endSq);

    return (victim * 8) + (4 - attacker);
}

// Returns a score from the initial capture
// This helps reduce the number of times SEE must be used in quiescence search,
// since if we have a losing trade after capture-recapture our opponent could
// likely stand pat, or we could've captured with a better piece
int Board::getExchangeScore(int color, Move m) {
    int endSq = getEndSq(m);
    int attacker = getPieceOnSquare(color, getStartSq(m));
    if (attacker == KINGS)
        attacker = -1;
    int victim = getPieceOnSquare(color^1, endSq);
    return victim - attacker;
}


//------------------------------------------------------------------------------
//-----------------------------MOVE GENERATION----------------------------------
//------------------------------------------------------------------------------

inline uint64_t Board::getWPawnSingleMoves(uint64_t pawns) {
    return (pawns << 8) & ~getOccupancy();
}

inline uint64_t Board::getBPawnSingleMoves(uint64_t pawns) {
    return (pawns >> 8) & ~getOccupancy();
}

uint64_t Board::getWPawnDoubleMoves(uint64_t pawns) {
    uint64_t open = ~getOccupancy();
    uint64_t temp = (pawns << 8) & open;
    pawns = (temp << 8) & open & RANKS[3];
    return pawns;
}

uint64_t Board::getBPawnDoubleMoves(uint64_t pawns) {
    uint64_t open = ~getOccupancy();
    uint64_t temp = (pawns >> 8) & open;
    pawns = (temp >> 8) & open & RANKS[4];
    return pawns;
}

inline uint64_t Board::getWPawnLeftCaptures(uint64_t pawns) {
    return (pawns << 7) & NOTH;
}

inline uint64_t Board::getBPawnLeftCaptures(uint64_t pawns) {
    return (pawns >> 9) & NOTH;
}

inline uint64_t Board::getWPawnRightCaptures(uint64_t pawns) {
    return (pawns << 9) & NOTA;
}

inline uint64_t Board::getBPawnRightCaptures(uint64_t pawns) {
    return (pawns >> 7) & NOTA;
}

inline uint64_t Board::getWPawnCaptures(uint64_t pawns) {
    return getWPawnLeftCaptures(pawns) | getWPawnRightCaptures(pawns);
}

inline uint64_t Board::getBPawnCaptures(uint64_t pawns) {
    return getBPawnLeftCaptures(pawns) | getBPawnRightCaptures(pawns);
}

inline uint64_t Board::getKnightSquares(int single) {
    return KNIGHTMOVES[single];
}

uint64_t Board::getBishopSquares(int single, uint64_t occ) {
    uint64_t *attTableLoc = magicBishops[single].table;
    occ &= magicBishops[single].mask;
    occ *= magicBishops[single].magic;
    occ >>= magicBishops[single].shift;
    return attTableLoc[occ];
}

uint64_t Board::getRookSquares(int single, uint64_t occ) {
    uint64_t *attTableLoc = magicRooks[single].table;
    occ &= magicRooks[single].mask;
    occ *= magicRooks[single].magic;
    occ >>= magicRooks[single].shift;
    return attTableLoc[occ];
}

uint64_t Board::getQueenSquares(int single, uint64_t occ) {
    return getBishopSquares(single, occ) | getRookSquares(single, occ);
}

inline uint64_t Board::getKingSquares(int single) {
    return KINGMOVES[single];
}

inline uint64_t Board::getOccupancy() {
    return allPieces[WHITE] | allPieces[BLACK];
}

inline int Board::epVictimSquare(int victimColor, uint16_t file) {
    return 8 * (3 + victimColor) + file;
}


//------------------------------------------------------------------------------
//---------------------------PUBLIC GETTER METHODS------------------------------
//------------------------------------------------------------------------------

bool Board::getWhiteCanKCastle() {
    return castlingRights & WHITEKSIDE;
}

bool Board::getBlackCanKCastle() {
    return castlingRights & BLACKKSIDE;
}

bool Board::getWhiteCanQCastle() {
    return castlingRights & WHITEQSIDE;
}

bool Board::getBlackCanQCastle() {
    return castlingRights & BLACKQSIDE;
}

uint16_t Board::getEPCaptureFile() {
    return epCaptureFile;
}

uint8_t Board::getFiftyMoveCounter() {
    return fiftyMoveCounter;
}

uint16_t Board::getMoveNumber() {
    return moveNumber;
}

int Board::getPlayerToMove() {
    return playerToMove;
}

uint64_t Board::getPieces(int color, int piece) {
    return pieces[color][piece];
}

uint64_t Board::getAllPieces(int color) {
    return allPieces[color];
}

int *Board::getMailbox() {
    int *result = new int[64];
    for (int i = 0; i < 64; i++) {
        result[i] = -1;
    }
    for (int i = 0; i < 6; i++) {
        uint64_t bitboard = pieces[WHITE][i];
        while (bitboard) {
            result[bitScanForward(bitboard)] = i;
            bitboard &= bitboard - 1;
        }
    }
    for (int i = 0; i < 6; i++) {
        uint64_t bitboard = pieces[BLACK][i];
        while (bitboard) {
            result[bitScanForward(bitboard)] = 6 + i;
            bitboard &= bitboard - 1;
        }
    }
    return result;
}

uint64_t Board::getZobristKey() {
    return zobristKey;
}

void Board::initZobristKey(int *mailbox) {
    zobristKey = 0;
    for (int i = 0; i < 64; i++) {
        if (mailbox[i] != -1) {
            zobristKey ^= zobristTable[mailbox[i] * 64 + i];
        }
    }
    if (playerToMove == BLACK)
        zobristKey ^= zobristTable[768];
    zobristKey ^= zobristTable[769 + castlingRights];
    zobristKey ^= zobristTable[785 + epCaptureFile];
}

// Dumb7Fill
uint64_t fillRayRight(uint64_t rayPieces, uint64_t empty, int shift) {
    uint64_t flood = rayPieces;
    // To prevent overflow across the sides of the board on east/west fills
    uint64_t borderMask = 0xFFFFFFFFFFFFFFFF;
    if (shift == 1 || shift == 9)
        borderMask = NOTH;
    else if (shift == 7)
        borderMask = NOTA;
    empty &= borderMask;
    flood |= rayPieces = (rayPieces >> shift) & empty;
    flood |= rayPieces = (rayPieces >> shift) & empty;
    flood |= rayPieces = (rayPieces >> shift) & empty;
    flood |= rayPieces = (rayPieces >> shift) & empty;
    flood |= rayPieces = (rayPieces >> shift) & empty;
    flood |=         (rayPieces >> shift) & empty;
    return           (flood >> shift) & borderMask;
}

uint64_t fillRayLeft(uint64_t rayPieces, uint64_t empty, int shift) {
    uint64_t flood = rayPieces;
    // To prevent overflow across the sides of the board on east/west fills
    uint64_t borderMask = 0xFFFFFFFFFFFFFFFF;
    if (shift == 1 || shift == 9)
        borderMask = NOTA;
    else if (shift == 7)
        borderMask = NOTH;
    empty &= borderMask;
    flood |= rayPieces = (rayPieces << shift) & empty;
    flood |= rayPieces = (rayPieces << shift) & empty;
    flood |= rayPieces = (rayPieces << shift) & empty;
    flood |= rayPieces = (rayPieces << shift) & empty;
    flood |= rayPieces = (rayPieces << shift) & empty;
    flood |=         (rayPieces << shift) & empty;
    return           (flood << shift) & borderMask;
}


//------------------------------------------------------------------------------
//-----------------------------MAGIC BITBOARDS----------------------------------
//------------------------------------------------------------------------------
// This code is adapted from Tord Romstad's approach to finding magics,
// available online at https://chessprogramming.wikispaces.com/Looking+for+Magics

// Maps an index from 0 from 2^nBits - 1 into one of the
// 2^nBits possible masks
uint64_t indexToMask64(int index, int nBits, uint64_t mask) {
    uint64_t result = 0;
    // For each bit in the mask
    for (int i = 0; i < nBits; i++) {
        int j = bitScanForward(mask);
        mask &= mask - 1;
        // If this bit should be set...
        if (index & INDEX_TO_BIT[i])
            // Set it at the same position in the result
            result |= INDEX_TO_BIT[j];
    }
    return result;
}

// Gets rook attacks using Dumb7Fill methods
uint64_t ratt(int sq, uint64_t block) {
    return fillRayRight(INDEX_TO_BIT[sq], ~block, NORTH_SOUTH_FILL) // south
         | fillRayLeft(INDEX_TO_BIT[sq], ~block, NORTH_SOUTH_FILL)  // north
         | fillRayLeft(INDEX_TO_BIT[sq], ~block, EAST_WEST_FILL)    // east
         | fillRayRight(INDEX_TO_BIT[sq], ~block, EAST_WEST_FILL);  // west
}

// Gets bishop attacks using Dumb7Fill methods
uint64_t batt(int sq, uint64_t block) {
    return fillRayLeft(INDEX_TO_BIT[sq], ~block, NE_SW_FILL)   // northeast
         | fillRayLeft(INDEX_TO_BIT[sq], ~block, NW_SE_FILL)   // northwest
         | fillRayRight(INDEX_TO_BIT[sq], ~block, NE_SW_FILL)  // southwest
         | fillRayRight(INDEX_TO_BIT[sq], ~block, NW_SE_FILL); // southeast
}

// Maps a mask using a candidate magic into an index nBits long
int magicMap(uint64_t masked, uint64_t magic, int nBits) {
    return (int) ((masked * magic) >> (64 - nBits));
}

/**
 * @brief Finds a magic number for the given square using trial and error.
 * @param sq The square to find the magic for.
 * @param iBits The length of the desired index, in bits
 * @param isBishop True for bishop magics, false for rook magics
 * @param magicRNG A random number generator to get magic candidates
 */
uint64_t findMagic(int sq, int iBits, bool isBishop) {
    uint64_t mask, maskedBits[4096], attSet[4096], used[4096], magic;
    bool failed;

    mask = isBishop ? BISHOP_MASK[sq] : ROOK_MASK[sq];
    int nBits = count(mask);
    // For each possible masked occupancy, get the attack set corresponding to
    // that square and occupancy
    for (int i = 0; i < (1 << nBits); i++) {
        maskedBits[i] = indexToMask64(i, nBits, mask);
        attSet[i] = isBishop ? batt(sq, maskedBits[i]) : ratt(sq, maskedBits[i]);
    }
    // Try 100 mill iterations before giving up
    for (int k = 0; k < 100000000; k++) {
        // Get a random magic candidate
        // We make this random 64-bit integer sparse by &-ing 3 random numbers
        // Sparse numbers are beneficial to keep the multiplied bits from
        // bleeding together and becoming garbage
        magic = magicRNG() & magicRNG() & magicRNG();
        // We want a large number of high bits to get a higher success rate,
        // since mask * magic is shifted by 64 - n bits, leaving n bits at the
        // end. Thus, anything but the top 12 bits (for rooks in the corners) or
        // less (for bishops) is garbage. Having a large number of high bits
        // when multiplying by the full mask gives a better spread of values
        // with different partial masks.
        if (count((mask * magic) & 0xFFF0000000000000ULL) < 10)
            continue;

        // Clear the used table
        for (int i = 0; i < 4096; i++)
            used[i] = 0;
        // Calculate the packed bits for every possible mask using this magic
        // and see if any fail
        failed = false;
        for (int i = 0; !failed && i < (1 << nBits); i++) {
            int mappedIndex = magicMap(maskedBits[i], magic, iBits);
            // No collision, mark the index as used for the given attack set
            if (!used[mappedIndex])
                used[mappedIndex] = attSet[i];
            // Otherwise, check for a constructive collsion, where a different
            // occupancy has the same attack set.
            // If the collision is not constructive, then we failed.
            else if (used[mappedIndex] != attSet[i])
                failed = true;
        }
        // If there were no collisions in all 2^nBits mappings, we have found
        // a valid magic
        if (!failed)
            return magic;
    }

    // Otherwise we failed :(
    // (this should never happen)
    return 0;
}