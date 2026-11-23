// Script-facing text fields must retain edits before Enter or Save is pressed.
#include <imgui.h>
#include <tut/tut.hpp>

#include <string>

bool AS_ImGui_InputText2(const std::string&, std::string&, int, int);

namespace tut {
struct datatextdraft {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    std::string value = "Before";
    ImVec2 button_position;
    datatextdraft() {
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(640, 480);
        io.DeltaTime = 1.0f / 60.0f;
        io.KeyMap[ImGuiKey_Enter] = 13;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~datatextdraft() {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    bool Frame(bool focus = false) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 20));
        ImGui::SetNextWindowSize(ImVec2(500, 300));
        ImGui::Begin("Draft test");
        if (focus) ImGui::SetKeyboardFocusHere();
        const bool submitted = AS_ImGui_InputText2("Name", value, 128, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Button("Save");
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        button_position = ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        ImGui::End();
        ImGui::Render();
        return submitted;
    }
};
typedef test_group<datatextdraft> tg_textdraft;
tg_textdraft test_group_textdraft("ImGuiTextDraft");
typedef tg_textdraft::object textdraft;

template <> template <>
void textdraft::test<1>() {
    set_test_name("typing and hovering Save preserve a draft without submitting it");
    Frame(true);
    Frame();
    // Keyboard focus selects the original value, as in the game's name form.
    ImGui::GetIO().AddInputCharactersUTF8("After");
    ensure("typing alone does not submit EnterReturnsTrue", !Frame());
    ensure_equals("the script can read the live draft", value, std::string("After"));
    ImGui::GetIO().MousePos = button_position;
    ensure("hovering Save does not submit", !Frame());
    ensure_equals("hover keeps the draft", value, std::string("After"));
    ImGui::GetIO().KeysDown[13] = true;
    ensure("Enter still reports submission", Frame());
    ensure_equals("submission keeps the same draft", value, std::string("After"));
}
}  // namespace tut
