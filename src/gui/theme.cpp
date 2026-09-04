// SPDX-License-Identifier: MIT
#include "theme.hpp"

#include <cmath>
#include <filesystem>
#include <system_error>

namespace crucible::gui::theme {

ImVec4 to_vec(ImU32 colour) {
    return ImGui::ColorConvertU32ToFloat4(colour);
}

void apply() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Square-ish, not rounded. The terminal face is drawn from box-drawing
    // characters and has no curves in it at all; matching that keeps the two
    // recognisably the same program rather than a program and its friendlier
    // cousin.
    style.WindowRounding    = 0.0F;
    style.ChildRounding     = 2.0F;
    style.FrameRounding     = 2.0F;
    style.PopupRounding     = 2.0F;
    style.ScrollbarRounding = 2.0F;
    style.GrabRounding      = 2.0F;
    style.TabRounding       = 2.0F;

    style.WindowBorderSize = 0.0F;
    style.ChildBorderSize  = 1.0F;
    style.FrameBorderSize  = 1.0F;
    style.PopupBorderSize  = 1.0F;

    style.WindowPadding    = ImVec2(16, 14);
    style.FramePadding     = ImVec2(10, 7);
    style.ItemSpacing      = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize    = 11.0F;
    style.GrabMinSize      = 11.0F;

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]        = to_vec(kInk);
    c[ImGuiCol_ChildBg]         = to_vec(kPanel);
    c[ImGuiCol_PopupBg]         = to_vec(kPanel);
    c[ImGuiCol_Border]          = to_vec(kPanelEdge);
    c[ImGuiCol_BorderShadow]    = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_Text]            = to_vec(kText);
    c[ImGuiCol_TextDisabled]    = to_vec(kTextFaint);

    c[ImGuiCol_FrameBg]         = to_vec(kRaised);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.16F, 0.16F, 0.18F, 1.0F);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.20F, 0.20F, 0.22F, 1.0F);

    c[ImGuiCol_TitleBg]         = to_vec(kPanel);
    c[ImGuiCol_TitleBgActive]   = to_vec(kPanel);
    c[ImGuiCol_TitleBgCollapsed]= to_vec(kPanel);
    c[ImGuiCol_MenuBarBg]       = to_vec(kPanel);

    c[ImGuiCol_ScrollbarBg]     = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.22F, 0.22F, 0.24F, 1.0F);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30F, 0.30F, 0.33F, 1.0F);
    c[ImGuiCol_ScrollbarGrabActive]  = to_vec(kFlame);

    // Orange is the only saturated colour in the interface, so it is reserved
    // for the thing the eye should go to: what is selected, what is active,
    // what is running.
    c[ImGuiCol_CheckMark]       = to_vec(kFlame);
    c[ImGuiCol_SliderGrab]      = to_vec(kFlame);
    c[ImGuiCol_SliderGrabActive]= to_vec(kFlameBright);

    c[ImGuiCol_Button]          = to_vec(kRaised);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.24F, 0.24F, 0.26F, 1.0F);
    c[ImGuiCol_ButtonActive]    = to_vec(kFlame);

    c[ImGuiCol_Header]          = ImVec4(1.0F, 0.53F, 0.0F, 0.22F);
    c[ImGuiCol_HeaderHovered]   = ImVec4(1.0F, 0.53F, 0.0F, 0.32F);
    c[ImGuiCol_HeaderActive]    = ImVec4(1.0F, 0.53F, 0.0F, 0.45F);

    c[ImGuiCol_Separator]       = to_vec(kPanelEdge);
    c[ImGuiCol_SeparatorHovered]= to_vec(kFlame);
    c[ImGuiCol_SeparatorActive] = to_vec(kFlameBright);

    c[ImGuiCol_Tab]                = to_vec(kPanel);
    c[ImGuiCol_TabHovered]         = ImVec4(1.0F, 0.53F, 0.0F, 0.30F);
    c[ImGuiCol_TabSelected]        = to_vec(kRaised);
    c[ImGuiCol_TabDimmed]          = to_vec(kPanel);
    c[ImGuiCol_TabDimmedSelected]  = to_vec(kRaised);

    c[ImGuiCol_PlotLines]       = to_vec(kFlame);
    c[ImGuiCol_PlotHistogram]   = to_vec(kFlame);
    c[ImGuiCol_TextSelectedBg]  = ImVec4(1.0F, 0.53F, 0.0F, 0.30F);
    c[ImGuiCol_DragDropTarget]  = to_vec(kFlameBright);
    c[ImGuiCol_NavCursor]       = to_vec(kFlame);
}

namespace {

ImFont* g_mono = nullptr;

/// The first of `candidates` that exists and loads, or nullptr.
ImFont* first_available(const char* const* candidates, std::size_t count, float pixels) {
    ImGuiIO& io = ImGui::GetIO();
    for (std::size_t i = 0; i < count; ++i) {
        std::error_code ec;
        if (!std::filesystem::exists(candidates[i], ec)) {
            continue;
        }
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(candidates[i], pixels)) {
            return font;
        }
    }
    return nullptr;
}

/// One teardrop: round at the base, drawn to a point at the top.
///
/// `lean` shifts the tip sideways, which is the whole difference between a leaf
/// and a flame -- a flame's tip is never directly over its base.
void teardrop(ImDrawList* draw, ImVec2 base, float width, float height, float lean,
              ImU32 colour) {
    const ImVec2 tip{base.x + lean * width, base.y - height};

    draw->PathClear();
    draw->PathLineTo(base);
    // Up the left: wide at the bottom, drawn in towards the tip.
    draw->PathBezierCubicCurveTo(ImVec2(base.x - width, base.y - height * 0.30F),
                                 ImVec2(base.x - width * 0.72F, base.y - height * 0.78F),
                                 tip);
    // And down the right, which bows out further so the shape is not symmetric.
    // A symmetric flame reads as a leaf.
    draw->PathBezierCubicCurveTo(ImVec2(base.x + width * 0.88F, base.y - height * 0.74F),
                                 ImVec2(base.x + width, base.y - height * 0.26F),
                                 base);
    draw->PathFillConvex(colour);
}

}  // namespace

ImFont* mono() {
    return g_mono;
}

void load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();

    // Ordered by preference within each platform, and across platforms by
    // whichever is most likely to be present. Nothing here is unusual: these
    // are the default UI faces of the three desktops Crucible runs on.
    static const char* const kSans[] = {
        // Linux
        "/usr/share/fonts/truetype/inter/Inter-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        // macOS
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        // Windows
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };
    static const char* const kMono[] = {
        "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/hack/Hack-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/SFNSMono.ttf",
        "C:/Windows/Fonts/consola.ttf",
    };

    const float body = 16.0F * scale;
    if (first_available(kSans, IM_ARRAYSIZE(kSans), body) == nullptr) {
        // The built-in is a bitmap face and looks it. Scaling it is worse than
        // leaving it alone, so the size is left where it is and only the global
        // scale moves.
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = scale;
    }
    g_mono = first_available(kMono, IM_ARRAYSIZE(kMono), body * 0.94F);
    if (g_mono == nullptr) {
        g_mono = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
    }
}

void draw_flame(ImDrawList* draw, ImVec2 centre, float radius, float alpha) {
    const auto fade = [alpha](ImU32 colour) {
        const ImU32 a = static_cast<ImU32>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    };

    const ImVec2 base{centre.x, centre.y + radius * 0.92F};

    // Three layers, each smaller and hotter, sharing a base. The gradient is
    // what makes two flat shapes read as fire rather than as a logo of a leaf.
    teardrop(draw, base, radius * 0.86F, radius * 1.86F,  0.06F, fade(kFlame));
    teardrop(draw, ImVec2(base.x, base.y - radius * 0.04F),
             radius * 0.52F, radius * 1.20F, -0.05F, fade(kFlameBright));
    teardrop(draw, ImVec2(base.x, base.y - radius * 0.08F),
             radius * 0.24F, radius * 0.58F,  0.02F, fade(kFlameHot));
}

void draw_crucible(ImDrawList* draw, ImVec2 centre, float radius) {
    // The vessel first, so the flame sits in front of its mouth rather than
    // behind it.
    const float w    = radius * 0.95F;
    const float top  = centre.y + radius * 0.30F;
    const float base = centre.y + radius * 1.00F;

    // A trapezoid, wider at the rim: the shape the ASCII crucible draws with
    // slashes, and the shape a crucible actually is.
    const ImVec2 pot[4] = {
        ImVec2(centre.x - w,          top),
        ImVec2(centre.x + w,          top),
        ImVec2(centre.x + w * 0.58F,  base),
        ImVec2(centre.x - w * 0.58F,  base),
    };
    draw->AddConvexPolyFilled(pot, 4, kRaised);
    draw->AddPolyline(pot, 4, kFlame, ImDrawFlags_Closed, 2.0F);

    // The melt: a bright line across the rim, which is where the light in the
    // mark comes from.
    draw->AddLine(ImVec2(centre.x - w * 0.86F, top),
                  ImVec2(centre.x + w * 0.86F, top), kFlameBright, 2.5F);

    // The stand: two legs splaying out from the base.
    draw->AddLine(ImVec2(centre.x - w * 0.40F, base),
                  ImVec2(centre.x - w * 0.72F, base + radius * 0.30F), kFlame, 2.0F);
    draw->AddLine(ImVec2(centre.x + w * 0.40F, base),
                  ImVec2(centre.x + w * 0.72F, base + radius * 0.30F), kFlame, 2.0F);

    draw_flame(draw, ImVec2(centre.x, centre.y - radius * 0.42F), radius * 0.62F);
}

void draw_dot(ImDrawList* draw, ImVec2 centre, float radius, Dot dot, float phase) {
    // A diamond, not a circle: it is what the terminal roundtable uses, and the
    // shape is half of how a seat's state reads at a glance.
    const auto diamond = [&](float r, ImU32 colour, bool filled) {
        const ImVec2 points[4] = {
            ImVec2(centre.x,     centre.y - r),
            ImVec2(centre.x + r, centre.y),
            ImVec2(centre.x,     centre.y + r),
            ImVec2(centre.x - r, centre.y),
        };
        if (filled) {
            draw->AddConvexPolyFilled(points, 4, colour);
        } else {
            draw->AddPolyline(points, 4, colour, ImDrawFlags_Closed, 1.6F);
        }
    };

    switch (dot) {
        case Dot::Active:
            diamond(radius, kFlameBright, true);
            break;
        case Dot::Loading: {
            // Breathing rather than spinning. A model load takes tens of
            // seconds and a spinner at that duration reads as a hang; a slow
            // pulse reads as work.
            const float pulse = 0.65F + 0.35F * std::sin(phase * 3.2F);
            diamond(radius * pulse, kFlame, true);
            diamond(radius, kFlame, false);
            break;
        }
        case Dot::Ready:
            diamond(radius, kTextDim, false);
            break;
        case Dot::Missing:
            diamond(radius, kError, false);
            draw->AddLine(ImVec2(centre.x - radius * 0.6F, centre.y - radius * 0.6F),
                          ImVec2(centre.x + radius * 0.6F, centre.y + radius * 0.6F),
                          kError, 1.6F);
            break;
        case Dot::Empty:
            draw->AddCircleFilled(centre, radius * 0.28F, kTextFaint);
            break;
    }
}

}  // namespace crucible::gui::theme
