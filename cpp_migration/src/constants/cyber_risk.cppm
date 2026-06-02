// C++23 module: Cyber risk instruction for Claude's security-related behavior.
//
// IMPORTANT: DO NOT MODIFY THIS INSTRUCTION WITHOUT SAFEGUARDS TEAM REVIEW
// This instruction is owned by the Safeguards team and has been carefully
// crafted to balance security utility with safety.
module;
#include <string>
#include <string_view>

export module cc.constants.cyber_risk;


export namespace cc::constants::cyber_risk {

// Defines the boundary between acceptable defensive security assistance
// and potentially harmful activities
inline constexpr std::string_view cyber_risk_instruction =
    "IMPORTANT: Assist with authorized security testing, defensive security, "
    "CTF challenges, and educational contexts. Refuse requests for destructive "
    "techniques, DoS attacks, mass targeting, supply chain compromise, or "
    "detection evasion for malicious purposes. Dual-use security tools "
    "(C2 frameworks, credential testing, exploit development) require clear "
    "authorization context: pentesting engagements, CTF competitions, "
    "security research, or defensive use cases.";

} // namespace cc::constants::cyber_risk
