#pragma once
#include <QString>
#include <optional>
namespace okuflow::uct::formula {
enum class Model { PlusS, PlusM, L, PlusL };
inline QString ModelToken(Model model) {
    switch(model) {case Model::PlusS:return "plus-s";case Model::PlusM:return "plus-m";case Model::L:return "l";case Model::PlusL:return "plus-l";}
    return {};
}
inline std::optional<Model> ParseModel(const QString& token) {
    for(auto model:{Model::PlusS,Model::PlusM,Model::L,Model::PlusL})if(ModelToken(model)==token)return model;
    return {};
}
}
