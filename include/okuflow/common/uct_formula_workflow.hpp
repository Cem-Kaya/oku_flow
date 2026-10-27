#pragma once
#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/common/uct_formula.hpp"
namespace okuflow::uct::formula {
// Called only by the existing UCT worker. Source pixels have already received
// the ordinary rotation/stabilization/keystone processing, exactly once.
FormulaWork Work(std::shared_ptr<Engine> engine,const QRect& selectedRegion);
}
