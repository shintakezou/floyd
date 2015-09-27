
/*----------------------------------------------------------------------+
 |                                                                      |
 |      search.c                                                        |
 |                                                                      |
 +----------------------------------------------------------------------*/

/*----------------------------------------------------------------------+
 |      Includes                                                        |
 +----------------------------------------------------------------------*/

// C standard
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// System
#include <unistd.h>

// C extension
#include "cplus.h"

// Own interface
#include "Board.h"
#include "Engine.h"

// Other modules
#include "evaluate.h"

/*----------------------------------------------------------------------+
 |      Definitions                                                     |
 +----------------------------------------------------------------------*/

/*----------------------------------------------------------------------+
 |      Data                                                            |
 +----------------------------------------------------------------------*/

static const int pieceValue[] = {
        [empty] = -1,
        [whiteKing]   = 27, [whiteQueen]  = 9, [whiteRook] = 5,
        [whiteBishop] = 3,  [whiteKnight] = 3, [whitePawn] = 1,
        [blackKing]   = 27, [blackQueen]  = 9, [blackRook] = 5,
        [blackBishop] = 3,  [blackKnight] = 3, [blackPawn] = 1,
};

static const int promotionValue[] = { 9, 5, 3, 3 };

/*----------------------------------------------------------------------+
 |      Functions                                                       |
 +----------------------------------------------------------------------*/

static int pvSearch(Engine_t self, int depth, int alpha, int beta, int pvIndex);
static int scout(Engine_t self, int depth, int alpha);
static int qSearch(Engine_t self, int alpha);

static int exchange(Board_t self, int move);
static int filterAndSort(Board_t self, int moveList[], int nrMoves, int moveFilter);
static int filterLegalMoves(Board_t self, int moveList[], int nrMoves);
static void moveToFront(int moveList[], int nrMoves, int move);

/*----------------------------------------------------------------------+
 |      rootSearch                                                      |
 +----------------------------------------------------------------------*/

static int globalSignal; // TODO: not thread-local, not very nice for a library

static void catchSignal(int signal)
{
        globalSignal = signal;
}

// TODO: aspiration search
void rootSearch(Engine_t self, int depth, double movetime, searchInfo_fn *infoFunction, void *infoData)
{
        double startTime = xclock();
        self->nodeCount = 0;
        self->rootPlyNumber = board(self)->plyNumber;

        jmp_buf env;
        self->setjmp_env = &env;
        globalSignal = 0;
        sig_t oldHandler = signal(SIGALRM, catchSignal);
        alarm(ceil(movetime));
        if (setjmp(env) == 0) {
                // start search
                bool stop = false;
                for (int iteration=0; iteration<=depth && !stop; iteration++) {
                        self->depth = iteration;
                        self->score = pvSearch(self, iteration, -maxInt, maxInt, 0);
                        self->seconds = xclock() - startTime;
                        if (self->pv.len > 0)
                                self->bestMove = self->pv.v[0];
                        if (infoFunction != null)
                                stop = infoFunction(infoData);
                }
        } else {
                // search aborted
                self->seconds = xclock() - startTime;
                self->pv.len = (self->pv.len > 0) && (self->pv.v[0] != self->bestMove);
                if (infoFunction != null)
                        (void) infoFunction(infoData);
        }
        signal(SIGALRM, oldHandler);
}

/*----------------------------------------------------------------------+
 |      ttWrite                                                         |
 +----------------------------------------------------------------------*/

// TODO: move to ttable.c
static int ttWrite(void *self, int depth, int alpha, int beta, int score)
{
        // DUMMY
        return score;
}

/*----------------------------------------------------------------------+
 |      endScore / drawScore                                            |
 +----------------------------------------------------------------------*/

// TODO: move to evaluate.h
static inline int endScore(Engine_t self, bool check)
{
        int rootDistance = board(self)->plyNumber - self->rootPlyNumber;
        return check ? -32000 + rootDistance : 0;
}

static inline int drawScore(Engine_t self)
{
        return 0;
}

/*----------------------------------------------------------------------+
 |      pvSearch                                                        |
 +----------------------------------------------------------------------*/

// TODO: repetitions
// TODO: ttable
// TODO: killers
// TODO: internal deepening
// TODO: reductions
static int pvSearch(Engine_t self, int depth, int alpha, int beta, int pvIndex)
{
        self->nodeCount++;
        if (repetition(board(self)))
                return drawScore(self);
        int check = inCheck(board(self));
        int moveFilter = minInt;
        int bestScore = minInt;
        err_t err = OK;

        if (depth == 0 && !check) {
                bestScore = evaluate(board(self));
                if (bestScore >= beta) {
                        self->pv.len = pvIndex;
                        return ttWrite(self, depth, alpha, beta, bestScore);
                }
                moveFilter = 0;
        }

        int moveList[maxMoves];
        int nrMoves = generateMoves(board(self), moveList);
        nrMoves = filterAndSort(board(self), moveList, nrMoves, moveFilter);
        nrMoves = filterLegalMoves(board(self), moveList, nrMoves); // easier for PVS

        // Search the first move with open alpha-beta window
        if (nrMoves > 0) {
                if (pvIndex < self->pv.len)
                        moveToFront(moveList, nrMoves, self->pv.v[pvIndex]); // follow the pv
                else
                        pushList(self->pv, moveList[0]);
                makeMove(board(self), moveList[0]);
                int newDepth = max(0, depth - 1 + check);
                int newAlpha = max(alpha, bestScore);
                int score = -pvSearch(self, newDepth, -beta, -newAlpha, pvIndex + 1);
                if (score > bestScore)
                        bestScore = score;
                else
                        self->pv.len = pvIndex; // quiescence
                undoMove(board(self));
        }

        // Search the others with zero window and reductions, research if needed
        int reduction = 0;
        for (int i=1; i<nrMoves && bestScore<beta; i++) {
                makeMove(board(self), moveList[i]);
                int newDepth = max(0, depth - 1 + check - reduction);
                int newAlpha = max(alpha, bestScore);
                int score = -scout(self, newDepth, -newAlpha - 1);
                if (score > bestScore) {
                        int pvLen = self->pv.len;
                        pushList(self->pv, moveList[i]);
                        int researchDepth = max(0, depth - 1 + check);
                        score = -pvSearch(self, researchDepth, -beta, -newAlpha, pvLen + 1);
                        if (score > bestScore) {
                                bestScore = score;
                                for (int j=0; pvLen+j<self->pv.len; j++)
                                        self->pv.v[pvIndex+j] = self->pv.v[pvLen+j];
                                self->pv.len -= pvLen - pvIndex;
                        } else
                                self->pv.len = pvLen; // research failed
                }
                undoMove(board(self));
        }

        if (bestScore == minInt)
                bestScore = endScore(self, check);

cleanup:
        return ttWrite(self, depth, alpha, beta, bestScore);
}

/*----------------------------------------------------------------------+
 |      scout                                                           |
 +----------------------------------------------------------------------*/

// TODO: repetitions
// TODO: ttable
// TODO: killers
// TODO: null move
// TODO: internal deepening
// TODO: futility
// TODO: reductions
// TODO: abort
static int scout(Engine_t self, int depth, int alpha)
{
        self->nodeCount++;
        if (repetition(board(self)))
                return drawScore(self);
        if (depth == 0)
                return qSearch(self, alpha);

        if (globalSignal)
                longjmp(*(jmp_buf *)self->setjmp_env, 1);

        int check = inCheck(board(self));
        int bestScore = minInt;

        int moveList[maxMoves];
        int nrMoves = generateMoves(board(self), moveList);
        nrMoves = filterAndSort(board(self), moveList, nrMoves, minInt);

        int reduction = 0;
        for (int i=0; i<nrMoves && bestScore<=alpha; i++) {
                makeMove(board(self), moveList[i]);
                if (wasLegalMove(board(self))) {
                        int newDepth = max(0, depth - 1 + check - reduction);
                        int score = -scout(self, newDepth, -(alpha+1));
                        bestScore = max(bestScore, score);
                }
                undoMove(board(self));
        }

        if (bestScore == minInt)
                bestScore = endScore(self, check);

        return ttWrite(self, depth, alpha, alpha+1, bestScore);
}

/*----------------------------------------------------------------------+
 |      qSearch                                                         |
 +----------------------------------------------------------------------*/

// TODO: repetitions
// TODO: ttable
static int qSearch(Engine_t self, int alpha)
{
        int check = inCheck(board(self));
        int bestScore = check ? minInt : evaluate(board(self));

        if (bestScore > alpha)
                return ttWrite(self, 0, alpha, alpha+1, bestScore);

        int moveList[maxMoves];
        int nrMoves = generateMoves(board(self), moveList);
        nrMoves = filterAndSort(board(self), moveList, nrMoves, check ? minInt : 0);

        for (int i=0; i<nrMoves && bestScore<=alpha; i++) {
                makeMove(board(self), moveList[i]);
                if (wasLegalMove(board(self))) {
                        self->nodeCount++;
                        int score = -qSearch(self, -(alpha+1));
                        bestScore = max(bestScore, score);
                }
                undoMove(board(self));
        }

        if (bestScore == minInt)
                bestScore = endScore(self, check);

        return ttWrite(self, 0, alpha, alpha+1, bestScore);
}

/*----------------------------------------------------------------------+
 |      exchange (not really "SEE" yet)                                 |
 +----------------------------------------------------------------------*/

static int exchange(Board_t self, int move)
{
        int from = from(move);
        int to = to(move);

        int victim = self->squares[to];
        int score = pieceValue[victim];

        if (self->xside->attacks[to] != 0) {
                int piece = self->squares[from];
                score -= pieceValue[piece];
        } else {
                if (isPromotion(self, from, to))
                        score += promotionValue[move >> promotionBits] - 1;
        }
        return score;
}

/*----------------------------------------------------------------------+
 |      filterAndSort                                                   |
 +----------------------------------------------------------------------*/

// Comparator for qsort: descending order of prescore
static int compareMoves(const void *ap, const void *bp)
{
        int a = *(const int*)ap;
        int b = *(const int*)bp;
        return (a < b) - (a > b);
}

// TODO: recognize safe checks
static int filterAndSort(Board_t self, int moveList[], int nrMoves, int moveFilter)
{
        int n = 0;
        for (int i=0; i<nrMoves; i++) {
                int moveScore = exchange(self, moveList[i]);
                if (moveScore >= moveFilter)
                        moveList[n++] = (moveScore << 16) + (moveList[i] & 0xffff);
        }

        qsort(moveList, n, sizeof(moveList[0]), compareMoves);

        for (int i=0; i<n; i++)
                moveList[i] &= 0xffff;

        return n;
}

/*----------------------------------------------------------------------+
 |      filterLegalMoves                                                |
 +----------------------------------------------------------------------*/

static int filterLegalMoves(Board_t self, int moveList[], int nrMoves)
{
        int j = 0;
        for (int i=0; i<nrMoves; i++) {
                makeMove(self, moveList[i]);
                if (wasLegalMove(self))
                        moveList[j++] = moveList[i];
                undoMove(self);
        }
        return j;
}

/*----------------------------------------------------------------------+
 |      moveToFront                                                     |
 +----------------------------------------------------------------------*/

static void moveToFront(int moveList[], int nrMoves, int move)
{
        for (int i=0; i<nrMoves; i++) {
                if (moveList[i] != move)
                        continue;
                memmove(&moveList[0], &moveList[1], i * sizeof(moveList[0]));
                moveList[0] = move;
                return;
        }
}

/*----------------------------------------------------------------------+
 |                                                                      |
 +----------------------------------------------------------------------*/

