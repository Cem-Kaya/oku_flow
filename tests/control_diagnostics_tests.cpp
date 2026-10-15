#include "okuflow/common/control_diagnostics.hpp"
#include <limits>
#include <iostream>

int main() {
    using namespace okuflow;
    const auto require = [](bool valid, const char* message) {
        if (!valid) std::cerr << message << '\n';
        return valid;
    };
    ControlDiagnosticSnapshot sample;
    sample.frame = {7,8,9,10,11,1234}; sample.readyAtMs = 1300;
    sample.converging = false; sample.keystoneQuadValid = true;
    const auto identity = sample.frame;
    DecodeControlDiagnosticValues(sample,1,1,true,0,2);
    if (!require(sample.held == false && sample.converging == false && sample.stabilizationModelValid,
        "A locked, ready live frame should permit bounded adjustment")) return 1;
    DecodeControlDiagnosticValues(sample,1,1,true,1,2);
    if (!require(sample.held == true, "A sharp retained frame must remain held")) return 1;
    DecodeControlDiagnosticValues(sample,1,4,true,2,2);
    if (!require(sample.held == true, "A recovery blend still contains retained pixels")) return 1;
    DecodeControlDiagnosticValues(sample,1,1,true,0,1);
    if (!require(!sample.held.has_value(), "An armed but unready hold buffer must be unknown")) return 1;
    sample.converging = false;
    DecodeControlDiagnosticValues(sample,0,1,false,0,0);
    if (!require(sample.converging == true && !sample.stabilizationModelValid,
        "A locked tag without a valid model must not report settled geometry")) return 1;
    sample.converging = true;
    DecodeControlDiagnosticValues(sample,1,1,false,0,0);
    if (!require(sample.converging == true, "Stable tripod must not hide pending keystone correction")) return 1;
    sample.converging.reset();
    DecodeControlDiagnosticValues(sample,1,1,false,0,0);
    if (!require(sample.converging == true, "Unknown geometry must not be converted to settled")) return 1;
    DecodeControlDiagnosticValues(sample,std::numeric_limits<float>::quiet_NaN(),1,true,0,2);
    if (!require(!sample.converging.has_value() && !sample.stabilizationModelValid,
        "Corrupt model diagnostics must not authorize adjustment")) return 1;
    sample.converging = false;
    DecodeControlDiagnosticValues(sample,1,0.75f,true,0.75f,2);
    if (!require(!sample.converging.has_value() && !sample.held.has_value(),
        "Invalid fractional state tags must not be rounded into valid modes")) return 1;
    if (!require(sample.frame == identity && sample.readyAtMs == 1300 && sample.keystoneQuadValid == true,
        "Decoding must preserve exact submission identity, age, and keystone provenance")) return 1;
    std::cout << "Completed control diagnostic semantics passed\n";
    return 0;
}
