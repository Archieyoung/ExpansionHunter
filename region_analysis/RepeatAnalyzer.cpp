//
// Expansion Hunter
// Copyright 2016-2019 Illumina, Inc.
// All rights reserved.
//
// Author: Egor Dolzhenko <edolzhenko@illumina.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#include "region_analysis/RepeatAnalyzer.hh"

#include "thirdparty/spdlog/spdlog.h"

#include "graphalign/GraphAlignmentOperations.hh"
#include "graphutils/SequenceOperations.hh"

#include "alignment/AlignmentFilters.hh"
#include "alignment/OperationsOnAlignments.hh"
#include "genotyping/RepeatGenotyper.hh"

namespace ehunter
{

using boost::optional;
using graphtools::GraphAlignment;
using graphtools::NodeId;
using graphtools::prettyPrint;
using graphtools::splitStringByDelimiter;
using std::list;
using std::string;
using std::unique_ptr;
using std::vector;

static bool checkIfAlignmentIsConfident(
    NodeId repeatNodeId, const GraphAlignment& alignment, const RepeatAlignmentStats& alignmentStats)
{
    if (!checkIfPassesAlignmentFilters(alignment))
    {
        return false;
    }

    const bool doesReadAlignWellOverLeftFlank = checkIfUpstreamAlignmentIsGood(repeatNodeId, alignment);
    const bool doesReadAlignWellOverRightFlank = checkIfDownstreamAlignmentIsGood(repeatNodeId, alignment);

    if (alignmentStats.canonicalAlignmentType() == AlignmentType::kFlanksRepeat)
    {
        if (!doesReadAlignWellOverLeftFlank && !doesReadAlignWellOverRightFlank)
        {
            return false;
        }
    }

    if (alignmentStats.canonicalAlignmentType() == AlignmentType::kSpansRepeat)
    {
        if (!doesReadAlignWellOverLeftFlank || !doesReadAlignWellOverRightFlank)
        {
            return false;
        }
    }

    return true;
}

void RepeatAnalyzer::processMates(
    const Read& read, const GraphAlignment& readAlignment, const Read& mate, const GraphAlignment& mateAlignment)
{
    RepeatAlignmentStats readAlignmentStats = classifyReadAlignment(readAlignment);
    RepeatAlignmentStats mateAlignmentStats = classifyReadAlignment(mateAlignment);

    const bool isReadAlignmentConfident
        = checkIfAlignmentIsConfident(repeatNodeId(), readAlignment, readAlignmentStats);
    const bool isMateAlignmentConfident
        = checkIfAlignmentIsConfident(repeatNodeId(), mateAlignment, mateAlignmentStats);

    if (isReadAlignmentConfident)
    {
        console_->trace(
            "{} is {} for variant {}", read.readId(), readAlignmentStats.canonicalAlignmentType(), variantId_);
        alignmentStatsCalculator_.inspect(readAlignment);
        summarizeAlignmentsToReadCounts(readAlignmentStats);
    }
    else
    {
        console_->debug(
            "Could not confidently aligned {} to repeat node {} of {}\n{}", read.readId(), repeatNodeId(), variantId_,
            prettyPrint(readAlignment, read.sequence()));
    }

    if (isMateAlignmentConfident)
    {
        console_->trace(
            "{} is {} for variant {}", mate.readId(), mateAlignmentStats.canonicalAlignmentType(), variantId_);
        alignmentStatsCalculator_.inspect(mateAlignment);
        summarizeAlignmentsToReadCounts(mateAlignmentStats);
    }
    else
    {
        console_->debug(
            "Could not confidently align {} to repeat node {} of {}\n{}", mate.readId(), repeatNodeId(), variantId_,
            prettyPrint(mateAlignment, mate.sequence()));
    }
}

RepeatAlignmentStats RepeatAnalyzer::classifyReadAlignment(const graphtools::GraphAlignment& alignment)
{
    AlignmentType alignmentType = alignmentClassifier_.Classify(alignment);
    int numRepeatUnitsOverlapped = countFullOverlaps(repeatNodeId(), alignment);

    return RepeatAlignmentStats(alignment, alignmentType, numRepeatUnitsOverlapped);
}

void RepeatAnalyzer::summarizeAlignmentsToReadCounts(const RepeatAlignmentStats& repeatAlignmentStats)
{
    switch (repeatAlignmentStats.canonicalAlignmentType())
    {
    case AlignmentType::kSpansRepeat:
        countsOfSpanningReads_.incrementCountOf(repeatAlignmentStats.numRepeatUnitsSpanned());
        break;
    case AlignmentType::kFlanksRepeat:
        countsOfFlankingReads_.incrementCountOf(repeatAlignmentStats.numRepeatUnitsSpanned());
        break;
    case AlignmentType::kInsideRepeat:
        countsOfInrepeatReads_.incrementCountOf(repeatAlignmentStats.numRepeatUnitsSpanned());
        break;
    default:
        break;
    }
}

static vector<int32_t> generateCandidateAlleleSizes(
    const CountTable& spanningTable, const CountTable& flankingTable, const CountTable& inrepeatTable)
{
    vector<int32_t> candidateSizes = spanningTable.getElementsWithNonzeroCounts();
    const int32_t longestSpanning
        = candidateSizes.empty() ? 0 : *std::max_element(candidateSizes.begin(), candidateSizes.end());

    const vector<int32_t> flankingSizes = flankingTable.getElementsWithNonzeroCounts();
    const int32_t longestFlanking
        = flankingSizes.empty() ? 0 : *std::max_element(flankingSizes.begin(), flankingSizes.end());

    const vector<int32_t> inrepeatSizes = inrepeatTable.getElementsWithNonzeroCounts();
    const int32_t longestInrepeat
        = inrepeatSizes.empty() ? 0 : *std::max_element(inrepeatSizes.begin(), inrepeatSizes.end());

    const int32_t longestNonSpanning = std::max(longestFlanking, longestInrepeat);

    if (longestSpanning < longestNonSpanning)
    {
        candidateSizes.push_back(longestNonSpanning);
    }

    return candidateSizes;
}

unique_ptr<VariantFindings> RepeatAnalyzer::analyze(const LocusStats& stats) const
{
    optional<RepeatGenotype> repeatGenotype = boost::none;
    auto genotypeFilter = GenotypeFilter();
    CountTable spanningTable = countsOfSpanningReads_;
    CountTable flankingTable = countsOfFlankingReads_;
    CountTable inrepeatTable = countsOfInrepeatReads_;

    if (stats.meanReadLength() == 0 || stats.depth() < genotyperParams_.minLocusCoverage)
    {
        genotypeFilter = genotypeFilter | GenotypeFilter::kLowDepth;
    }
    else
    {
        const int32_t repeatUnitLength = repeatUnit_.length();
        const double propCorrectMolecules = 0.97;
        const int maxNumUnitsInRead = std::ceil(stats.meanReadLength() / static_cast<double>(repeatUnitLength));
        spanningTable = collapseTopElements(spanningTable, maxNumUnitsInRead);
        flankingTable = collapseTopElements(flankingTable, maxNumUnitsInRead);
        inrepeatTable = collapseTopElements(inrepeatTable, maxNumUnitsInRead);

        const vector<int32_t> candidateAlleleSizes
            = generateCandidateAlleleSizes(spanningTable, flankingTable, inrepeatTable);

        const double haplotypeDepth = stats.alleleCount() == AlleleCount::kTwo ? stats.depth() / 2 : stats.depth();
        const int minBreakpointSpanningReads = stats.alleleCount() == AlleleCount::kTwo
            ? genotyperParams_.minBreakpointSpanningReads
            : (genotyperParams_.minBreakpointSpanningReads / 2);

        RepeatGenotyper repeatGenotyper(
            haplotypeDepth, stats.alleleCount(), repeatUnitLength, maxNumUnitsInRead, propCorrectMolecules,
            spanningTable, flankingTable, inrepeatTable, countOfInrepeatReadPairs_);
        repeatGenotype = repeatGenotyper.genotypeRepeat(candidateAlleleSizes);

        GraphVariantAlignmentStats alignmentStats = alignmentStatsCalculator_.getStats();
        if (alignmentStats.numReadsSpanningRightBreakpoint() < minBreakpointSpanningReads
            || alignmentStats.numReadsSpanningLeftBreakpoint() < minBreakpointSpanningReads)
        {
            genotypeFilter = genotypeFilter | GenotypeFilter::kLowDepth;
        }
    }

    unique_ptr<VariantFindings> variantFindingsPtr(new RepeatFindings(
        spanningTable, flankingTable, inrepeatTable, stats.alleleCount(), repeatGenotype, genotypeFilter));
    return variantFindingsPtr;
}

}
