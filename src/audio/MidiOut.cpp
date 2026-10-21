#include "audio/MidiOut.h"

bool MidiOut::OpenConfigured()
{
    m_pOut.reset();
    m_pOut = MidiOutputs::OpenConfigured();
    return m_pOut != nullptr;
}
