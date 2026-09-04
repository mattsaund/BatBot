// SPDX-License-Identifier: MIT
//
// See scroll.hpp.
#include "crucible/ui/widgets/scroll.hpp"

#include <utility>

#include <ftxui/dom/node.hpp>

namespace crucible::ui {

ftxui::Element measure_height(ftxui::Element child, int* height) {
    // Derived from Node rather than FTXUI's own NodeDecorator, whose header is
    // private to its source tree. The two methods below are what that class
    // does; everything else is inherited.
    class Measure : public ftxui::Node {
    public:
        Measure(ftxui::Element child, int* height)
            : Node({std::move(child)}), height_(height) {}

        void ComputeRequirement() override {
            Node::ComputeRequirement();
            requirement_ = children_[0]->requirement();
        }

        void SetBox(ftxui::Box box) override {
            Node::SetBox(box);
            children_[0]->SetBox(box);
            if (height_ != nullptr) {
                *height_ = box.y_max - box.y_min + 1;
            }
        }

    private:
        int* height_;
    };

    return std::make_shared<Measure>(std::move(child), height);
}

}  // namespace crucible::ui
