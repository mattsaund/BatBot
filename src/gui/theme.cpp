// SPDX-License-Identifier: MIT
#include "theme.hpp"

#include "fonts.hpp"

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

ImFont* g_body    = nullptr;
ImFont* g_bold    = nullptr;
ImFont* g_italic  = nullptr;
ImFont* g_heading = nullptr;

/// The first of `candidates` that exists and loads, or nullptr.
ImFont* first_available(const char* const* candidates, std::size_t count, float pixels,
                        const ImWchar* ranges) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.GlyphRanges = ranges;
    for (std::size_t i = 0; i < count; ++i) {
        std::error_code ec;
        if (!std::filesystem::exists(candidates[i], ec)) {
            continue;
        }
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(candidates[i], pixels, &cfg)) {
            return font;
        }
    }
    return nullptr;
}

/// The flame outline, as one closed path.
///
/// A real flame silhouette rather than a leaf: it is asymmetric, the tip leans,
/// and the base curls inward to a notch. That notch is the whole difference
/// between something that reads as fire and something that reads as foliage,
/// and it is why this is a concave fill -- an earlier version was three stacked
/// convex teardrops, which had no notch and looked like a leaf with highlights.
///
/// `lean` scales the sideways offset of the tip, so the core can lean the other
/// way from the body. Coordinates are fractions of `radius` about `base`, which
/// is the point the flame stands on.
void flame_path(ImDrawList* draw, ImVec2 base, float radius, float lean) {
    const auto at = [&](float x, float y) {
        return ImVec2(base.x + x * radius, base.y + y * radius);
    };

    draw->PathClear();
    draw->PathLineTo(at(0.00F, 0.16F));                       // the notch
    // Down and out to the left foot. The notch is shallow on purpose: an
    // earlier version cut a third of the way up and the shape read as a paw.
    draw->PathBezierCubicCurveTo(at(-0.12F, 0.34F), at(-0.26F, 0.42F), at(-0.40F, 0.40F));
    // Up the left side in two sweeps, which is what gives it a waist. One long
    // curve from foot to tip bulges in the middle and looks like a leaf.
    draw->PathBezierCubicCurveTo(at(-0.62F, 0.06F), at(-0.52F, -0.20F), at(-0.34F, -0.42F));
    draw->PathBezierCubicCurveTo(at(-0.20F, -0.72F), at(-0.04F, -0.86F),
                                 at(0.10F * lean, -1.00F));   // the tip, leaning
    // And down the right, which is straighter -- a flame is not symmetric, and
    // making it so is the difference between fire and a teardrop.
    draw->PathBezierCubicCurveTo(at(0.28F, -0.74F), at(0.34F, -0.48F), at(0.34F, -0.24F));
    draw->PathBezierCubicCurveTo(at(0.33F, -0.02F), at(0.42F, 0.16F), at(0.42F, 0.36F));
    draw->PathBezierCubicCurveTo(at(0.32F, 0.42F), at(0.14F, 0.32F), at(0.00F, 0.16F));
}

}  // namespace

ImFont* body()    { return g_body; }
ImFont* bold()    { return g_bold != nullptr ? g_bold : g_body; }
ImFont* italic()  { return g_italic != nullptr ? g_italic : g_body; }
ImFont* heading() { return g_heading != nullptr ? g_heading : bold(); }

void load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const float size = 15.0F * scale;

    // The glyphs to bake, beyond Latin.
    //
    // ImGui's default range is Latin-1 and nothing else, so the first bulleted
    // list rendered every "\xE2\x80\xA2" as a question mark. It is not only the
    // bullet: instruction-tuned models emit em dashes, curly quotes and
    // ellipses constantly, and every one of those would have been a "?" in an
    // answer that looked fine in the terminal.
    //
    // General punctuation and the arrow block cover all of it, and cost a few
    // hundred glyphs in an atlas that already holds two hundred and fifty.
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF,  // Latin, Latin-1 supplement
        0x2010, 0x205E,  // dashes, quotes, bullet, ellipsis, dagger
        0x2190, 0x21FF,  // arrows
        0x2200, 0x22FF,  // maths operators, for a model showing its working
        0,
    };

#ifdef CRUCIBLE_HAS_EMBEDDED_FONT
    // Compiled in, so this cannot fail for want of a file. ImGui takes
    // ownership of a font buffer it is given and frees it on shutdown, which it
    // must not do to a static array -- FontDataOwnedByAtlas says so.
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.GlyphRanges          = kRanges;

    const auto embedded = [&](const unsigned char* data, unsigned int bytes, float pixels) {
        return io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char*>(data), static_cast<int>(bytes), pixels, &cfg);
    };
    g_body    = embedded(fonts::kRegular, fonts::kRegular_size, size);
    g_bold    = embedded(fonts::kBold,    fonts::kBold_size,    size);
    g_italic  = embedded(fonts::kItalic,  fonts::kItalic_size,  size);
    // Headings are the same face a size and a half up. Markdown structure has
    // to be visible while scrolling past it, and weight alone does not do that
    // in a monospace family where every glyph is already the same width.
    g_heading = embedded(fonts::kBold,    fonts::kBold_size,    size * 1.32F);
#endif

    if (g_body == nullptr) {
        // No embedded font in this build. A system monospace keeps the
        // interface honest -- Crucible's other face is a terminal, and mixing a
        // proportional font in here would read as a different program.
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
        g_body = first_available(kMono, IM_ARRAYSIZE(kMono), size, kRanges);
    }
    if (g_body == nullptr) {
        // The built-in is a bitmap face and looks it, but a window with ugly
        // text beats no window.
        g_body = io.Fonts->AddFontDefault();
        io.FontGlobalScale = scale;
    }
}

void draw_flame(ImDrawList* draw, ImVec2 centre, float radius, float alpha) {
    const auto fade = [alpha](ImU32 colour) {
        const ImU32 a = static_cast<ImU32>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    };

    // The same silhouette twice: the body, and a core two thirds the size
    // leaning the other way. Two shapes, because a flame is a shape with a
    // hotter middle and anything more is decoration.
    //
    // Nothing here moves. An earlier version flickered the whole mark with the
    // engine's pulse, which is the sort of thing that looks alive for a minute
    // and then makes a window impossible to sit next to.
    const ImVec2 base{centre.x, centre.y + radius * 0.62F};
    flame_path(draw, base, radius, 1.0F);
    draw->PathFillConcave(fade(kFlame));

    // The core sits on the same foot and leans the other way, so the two edges
    // cross rather than nest -- which is what makes it read as two tongues of
    // one flame instead of a shape with an outline.
    flame_path(draw, ImVec2(base.x, base.y - radius * 0.03F), radius * 0.46F, -0.9F);
    draw->PathFillConcave(fade(kFlameBright));
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

    draw_flame(draw, ImVec2(centre.x, centre.y - radius * 0.52F), radius * 0.68F);
}

void draw_dot(ImDrawList* draw, ImVec2 centre, float radius, Dot dot) {
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
        case Dot::Loading:
            // A filled core inside a ring: loading, and the percentage beside
            // it is what actually moves. This used to breathe with a sine, and
            // a mark that pulses for the whole minute a 30B model takes to load
            // is exhausting to sit beside.
            diamond(radius * 0.55F, kFlame, true);
            diamond(radius, kFlame, false);
            break;
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
