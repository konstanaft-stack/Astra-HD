#include "Framegen/OwnSelectionWitness.hpp"
#include <array>
#include <cassert>
#include <string>
#include <cstdio>
using namespace cvr::framegen;
struct SelectorResult {
 bool frame{},inputs_handled=true,pattern_available{};std::string requested_step="RR";
 struct {std::array<unsigned,2> expected_products{2,3};std::array<int64_t,2> newest_real_ns{11,12},newest_generated_ns{13,14};unsigned compatible_pairs=4,future_pairs=5,expired_pairs=6,skew_pairs=7,regressed_pairs=8,early_for_slot=9;int64_t minimum_candidate_skew_ns=10,minimum_candidate_age_ns=11,desired_content_ns=12,display_period_ns=11111111;}diagnostics;
};
#include "SelectorProductionHandoff.hpp"
int main(){
 OwnSelectionWitness witness;witness.attempt=1;SelectorResult selected;TestSelectorHandoff(witness,selected);
 const auto& d=witness.workerDecision;
 assert(d.freshNoCandidate&&d.requestedKnown&&d.requestedStep[0]=='R'&&d.requestedStep[1]=='R'&&d.inputsHandled&&!d.patternAvailable);
 assert((d.expectedProducts==std::array<unsigned,2>{2,3})&&d.newestReal[0]==11&&d.newestReal[1]==12&&d.newestGenerated[0]==13&&d.newestGenerated[1]==14);
 assert(d.compatible==4&&d.future==5&&d.expired==6&&d.skew==7&&d.regressed==8&&d.earlyForSlot==9&&d.minimumSkew==10&&d.minimumAge==11&&d.desiredContent==12&&d.period==11111111);
 selected.requested_step="GG";TestSelectorHandoff(witness,selected);assert(witness.workerDecision.requestedStep[0]=='G');
 selected.frame=true;TestSelectorHandoff(witness,selected);assert(!witness.workerDecision.freshNoCandidate);
 selected.frame=false;selected.inputs_handled=false;TestSelectorHandoff(witness,selected);assert(!witness.workerDecision.freshNoCandidate);
 selected.inputs_handled=true;selected.requested_step="";TestSelectorHandoff(witness,selected);assert(!witness.workerDecision.requestedKnown&&!witness.workerDecision.freshNoCandidate);
 selected.requested_step="RX";TestSelectorHandoff(witness,selected);assert(!witness.workerDecision.requestedKnown);
 selected.requested_step="RR";selected.diagnostics.display_period_ns=0;TestSelectorHandoff(witness,selected);assert(!witness.workerDecision.freshNoCandidate);
 witness={};selected.requested_step="RR";TestSelectorHandoff(witness,selected);assert(!witness.workerDecision.requestedKnown);
 std::puts("PASS actual Bridge selector handoff: exact existing diagnostics, RR/GG, no candidate, repeat/lock/unknown/disabled validity");
}
