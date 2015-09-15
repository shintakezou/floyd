#!/usr/bin/env python
#-----------------------------------------------------------------------
#
#       tune.py -- hill climbing of the evaluation vector
#
#-----------------------------------------------------------------------

import floyd as engine
import json
import math
import sys

#-----------------------------------------------------------------------
#       readTestsFromEpd
#-----------------------------------------------------------------------

def readTestsFromEpd(input):
        """Read test postions from EPD
        Each line must contain a 4-field FEN followed by a 'result' opcode and operand,
        optionally followed by 'white' and 'black' opcodes for the players' ratings."""

        tests = []
        target = { '1-0' : 1.0, '0-1' : 0.0, '1/2-1/2' : 0.5 }

        for line in input:
                line = line.strip().split()
                pos = ' '.join(line[0:4])
                result = line[5].rstrip(';')
                if line[6] == 'white' and line[8] == 'black':
                        whiteElo = int(line[7].rstrip(';'))
                        blackElo = int(line[9].rstrip(';'))
                        pos = '%s %d' % (pos, whiteElo - blackElo)
                tests.append((pos, target[result] if line[1] == 'w' else 1.0 - target[result]))

        return tests

#-----------------------------------------------------------------------
#       getVector
#-----------------------------------------------------------------------

def getVector():
        vector, names = [], []
        try:
                coef = 0
                while True:
                        value, id = engine.setCoefficient(coef, 0)
                        engine.setCoefficient(coef, value) # TODO: HACK
                        vector.append(value)
                        names.append(id)
                        coef += 1
        except IndexError:
                pass
        return vector, names

#-----------------------------------------------------------------------
#       evaluateVector
#-----------------------------------------------------------------------

def evaluateVector(tests, passive, fast):
        """
        tests: list of (fen, result) tuples
        passive: (in/out) dict of positions whose score hasn't moved yet
        fast: flag to indicate fast mode must be used
        """

        sumSquaredErrors = 0.0
        scores = []
        for pos, target in tests:
                if not fast or pos not in passive:
                        score, pv = engine.search(pos, 0) # slow
                        if pos in passive and score != passive[pos]:
                                del passive[pos]
                else:
                        score = passive[pos] # fast
                scores.append(score)
                p = scoreToP(score)
                sumSquaredErrors += (p - target) * (p - target) 

        return math.sqrt(sumSquaredErrors / len(tests)), scores

#-----------------------------------------------------------------------
#       scoreToP
#-----------------------------------------------------------------------

def scoreToP(score):
        """Convert a score in pawns to a winning probability (0..1)"""
        return 1 / (1 + 10 ** (-score/4.0))

#-----------------------------------------------------------------------
#       tuneSingle
#-----------------------------------------------------------------------

def tuneSingle(coef, tests, initialValue, initialResidual, initialScores):
        """Tune a single coefficient using robust a form of hill-climbing"""

        print 'evaluate id %s value %d residual %.9f' % (names[coef], initialValue, initialResidual)

        cache = { initialValue: initialResidual } # value -> residual

        bestValue, bestResidual, bestScores = initialValue, initialResidual, initialScores

        # Initial window scales magnitude of initial value
        sigmoid = 1.0 / (1.0 + math.exp(-initialValue * 1e-3))
        slope = sigmoid * (1.0 - sigmoid)
        window = 0.02 / max(slope, 0.01) * 1e3

        # Even is good as it increases the density around bestValue (1 ... 2 ... 3 .X. 4 ... 5 ... 6)
        # Using 6 steps gives 2 in the middle and 2 on the sides, so on each side
        # of bestValue at least two probes must be worse before breaking out.
        nrSteps = 6

        # For switching to fast mode
        positions = [item[0] for item in tests]
        passive = dict(zip(positions, bestScores))
        lastActive, streak = None, 0
        fast = False

        # Loop until an improvement is found or the search is exhausted
        exhausted = False
        while not exhausted:

                # Center the window around the current best value
                center = bestValue
                minValue, maxValue = center - window/2, center + window/2
                stepSize = window / (nrSteps - 1)

                # Walk through the range in equal steps. Always complete all steps
                exhausted = True
                for step in range(nrSteps):
                        nextValue = int(round(minValue + step * stepSize))
                        if nextValue in cache:
                                continue
                        exhausted = False

                        engine.setCoefficient(coef, nextValue)
                        nextResidual, nextScores = evaluateVector(tests, passive, fast)
                        cache[nextValue] = nextResidual

                        active = len(positions) - len(passive)
                        print 'evaluate id %s residual %.9f' % (names[coef], nextResidual),
                        if not fast:
                                print 'active %d' % active,
                        print 'value %d' % nextValue,

                        # Track changes in number of active positions for this parameter
                        if active != lastActive:
                                streak = 0
                        lastActive = active
                        streak += 1

                        # Determine if the result is an improvement
                        if (nextResidual, abs(nextValue)) < (bestResidual, abs(bestValue)):
                                bestValue, bestResidual, bestScores = nextValue, nextResidual, nextScores
                                print 'best'
                        else:
                                print # newline

                # Shrink the window if the best value is near the center
                if abs(bestValue - center) < window / 4:
                        if bestValue != initialValue:
                                break # Early termination (go to next parameter)
                        window /= 2
                        fast = streak >= nrSteps
                else:
                        if min(bestValue - minValue, maxValue - bestValue) < window / 8:
                                print 'widening'
                                window *= 1.5 # Slight increase window at edge of range

        # Update vector
        engine.setCoefficient(coef, bestValue)

        return bestValue, bestResidual, bestScores, active

#-----------------------------------------------------------------------
#       writeVector
#-----------------------------------------------------------------------

def writeVector(vector, filename):
        with open(filename, 'w') as fp:
                json.dump(dict(zip(names, vector)), fp, indent=1, separators=(',', ': '), sort_keys=True)
                fp.write('\n')

#-----------------------------------------------------------------------
#       main
#-----------------------------------------------------------------------

if __name__ == '__main__':

        # -- Step 0: Get vector definition from module

        vector, names = getVector()

        # -- Step 1: Parse command line arguments

        if len(sys.argv) == 1:
                print 'Usage: python tune.py [ -n <cpu> ] <vector> [ <parameter> ... ]'
                print 'Arguments:'
                print '    cpu            - (not implemented)'
                print '    vector         - JSON file to tune (input/output)'
                print '    parameter ...  - names of parameter(s) to tune (empty means all)'
                sys.exit(0)

        argi = 1

        if len(sys.argv) >= argi and sys.argv[argi] == '-n':
                cpu = int(sys.argv[argi+1])
                argi += 2

        # Read vector from file, if any
        filename = sys.argv[argi]
        argi += 1
        try:
                with open(filename, 'r') as fp:
                        values = dict(zip(names, vector))
                        values.update(json.load(fp))
                        vector = [values[name] for name in names]
                        for coef in range(len(vector)):
                                engine.setCoefficient(coef, vector[coef])
        except IOError as err:
                print err
                print 'continue'

        coefList = range(len(vector))
        if len(sys.argv) > argi:
                coefList = [coef for coef in coefList if names[coef] in sys.argv[argi:]]

        # -- Step 2: Read positions from stdin

        tests = readTestsFromEpd(sys.stdin)
        print 'positions count %d' % len(tests)

        # -- Step 3: Prepare. Calculate initial scores and residual

        bestResidual, bestScores = evaluateVector(tests, {}, False)
        print 'vector filename %s residual %.9f' % (repr(filename), bestResidual)
        print

        # -- Step 4: Tune all, half the set and repeat tuning until no more halving

        nrRounds = 0
        exitValue = 1
        exhausted = False
        while len(coefList) > 0 and not exhausted:
                nrRounds += 1
                print 'round %d count %d' % (nrRounds, len(coefList))
                print

                deltas = {}
                exhausted = True
                for coef in coefList:
                        oldValue = vector[coef]
                        newValue, newResidual, newScores, active = tuneSingle(coef, tests, oldValue, bestResidual, bestScores)

                        deltaResidual = newResidual - bestResidual
                        deltas[coef] = deltaResidual

                        if newValue != oldValue:
                                print 'update id %s residual %.9f delta %.3e active %d oldValue %d newValue %d' % (
                                        names[coef], newResidual, deltaResidual, active, oldValue, newValue)
                                vector[coef] = newValue
                                writeVector(vector, filename)
                                exitValue = 0
                                bestResidual, bestScores = newResidual, newScores
                                exhausted = False
                        print

                # Keep the most volatile half for the next round
                coefList = sorted(coefList, key=lambda x:deltas[x])
                newLen = len(coefList) // 2
                coefList = coefList[:newLen]

        # -- Step 5: Report result and exit

        print 'vector filename %s residual %.9f' % (repr(filename), bestResidual)
        print

        sys.exit(exitValue)

#-----------------------------------------------------------------------
#
#-----------------------------------------------------------------------

