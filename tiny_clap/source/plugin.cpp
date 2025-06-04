#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"
#include "clap/helpers/host-proxy.hh"
#include "clap/helpers/host-proxy.hxx"

#include "plugin.h"

namespace tiny {

Plugin::Plugin(const clap_host* host)
    : clap::helpers::Plugin<MisbehaviourHandler::Terminate, CheckingLevel::Maximal>(&descriptor, host)
{}

Plugin::~Plugin()
{}

}