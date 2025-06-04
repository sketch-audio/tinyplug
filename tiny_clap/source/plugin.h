#pragma once

#include "clap/helpers/plugin.hh"

#include "descriptor.h" 

namespace tiny {

using MisbehaviourHandler = clap::helpers::MisbehaviourHandler;
using CheckingLevel = clap::helpers::CheckingLevel;

class Plugin : public clap::helpers::Plugin<MisbehaviourHandler::Terminate, CheckingLevel::Maximal> {
public:

    Plugin(const clap_host* host);
    ~Plugin();

};

}