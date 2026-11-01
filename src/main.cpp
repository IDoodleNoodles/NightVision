#include "version.h"

#include <SimpleIni.h>

#include <mmsystem.h>
#include <Scaleform/G/GFx_MovieDef.h>

// SimpleIni brings in windows.h, whose ERROR macro would break REX::ERROR.
#undef ERROR

// Evidence (Ghidra, Fallout4 1.11.240; the call and the fields re-checked on the 1.10.163 executable):
//
//   Every frame Main::OnIdle_UpdateImageSpace calls ImageSpaceManager::ResetToBaseData, which copies
//   the weather's base data (or the override at +68) into currentEOFData.baseData at +80. It then walks
//   the active imagespace modifiers, which blend on top of that copy through ApplyModifier.
//   ImageSpaceEffectHDR::UpdateParams reads saturation, brightness, contrast and tint straight from
//   currentEOFData for the tonemap shader, so this is the one place that colours the whole picture.
//
//   Writing to the +68 override was rejected: seven call sites set or clear it for menus and scenes.
//   Instead a call hook on ResetToBaseData edits the fresh copy, which leaves those users alone and
//   lets hit, scope and radiation modifiers still apply on top. It also needs no plugin file. The hook
//   only runs game code that is already there when the vision is off, so an idle plugin costs one
//   branch per frame. It doubles as the tick, on the main thread, and it keeps running in menus.
//
//   Fallout4.esm has zd_ScopeNightVision (the vanilla scope: tint 0.31, 0.82, 0.22 at 0.69) but no
//   thermal form of any kind. The nearest is DetectLifeFXS and DetectLifeHostileFXS, the Recon and
//   Detect Life glow, so thermal is a cold grey picture with those shaders on living actors.
//
//   TESObjectREFR::ApplyEffectShader puts a timed ShaderReferenceEffect on the actor. Effects are
//   written to the save, so they run on a short lifetime that each scan extends instead of forever,
//   and any that are saved mid-use run out by themselves.
namespace Keys
{
	struct Key
	{
		RE::INPUT_DEVICE device{ RE::INPUT_DEVICE::kNone };
		std::int32_t     code{ -1 };

		[[nodiscard]] bool IsSet() const noexcept { return device != RE::INPUT_DEVICE::kNone; }
		[[nodiscard]] bool Matches(const RE::ButtonEvent& a_event) const noexcept
		{
			return IsSet() && a_event.device.get() == device && a_event.idCode == code;
		}
		[[nodiscard]] bool operator==(const Key&) const noexcept = default;
	};

	struct Name
	{
		std::string_view text;
		std::int32_t     code;
	};

	inline constexpr std::array NAMED_KEYS{
		Name{ "Backspace", 0x08 }, Name{ "Tab", 0x09 }, Name{ "Enter", 0x0D }, Name{ "Escape", 0x1B },
		Name{ "CapsLock", 0x14 }, Name{ "Space", 0x20 }, Name{ "PageUp", 0x21 }, Name{ "PageDown", 0x22 },
		Name{ "End", 0x23 }, Name{ "Home", 0x24 }, Name{ "Left", 0x25 }, Name{ "Up", 0x26 },
		Name{ "Right", 0x27 }, Name{ "Down", 0x28 }, Name{ "Insert", 0x2D }, Name{ "Delete", 0x2E },
		Name{ "LShift", 0xA0 }, Name{ "RShift", 0xA1 }, Name{ "LCtrl", 0xA2 }, Name{ "RCtrl", 0xA3 },
		Name{ "LAlt", 0xA4 }, Name{ "RAlt", 0xA5 }, Name{ "Minus", 0xBD }, Name{ "Equals", 0xBB },
		Name{ "Tilde", 0xC0 }
	};

	[[nodiscard]] std::string Lower(std::string_view a_text)
	{
		std::string out{ a_text };
		std::ranges::transform(out, out.begin(), [](unsigned char a_char) { return static_cast<char>(std::tolower(a_char)); });
		return out;
	}

	[[nodiscard]] std::string Trim(std::string_view a_text)
	{
		const auto first = a_text.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos) {
			return {};
		}
		return std::string{ a_text.substr(first, a_text.find_last_not_of(" \t\r\n") - first + 1) };
	}

	// nullopt = the text is not a key. An empty text is a valid "not set".
	[[nodiscard]] std::optional<Key> Parse(std::string_view a_text)
	{
		const auto text = Trim(a_text);
		if (text.empty()) {
			return Key{};
		}
		if (text.size() == 1 && std::isalnum(static_cast<unsigned char>(text[0]))) {
			return Key{ RE::INPUT_DEVICE::kKeyboard, std::toupper(static_cast<unsigned char>(text[0])) };
		}

		const auto lower = Lower(text);
		for (const auto& [name, code] : NAMED_KEYS) {
			if (Lower(name) == lower) {
				return Key{ RE::INPUT_DEVICE::kKeyboard, code };
			}
		}
		if (lower.size() >= 2 && lower.size() <= 3 && lower[0] == 'f') {
			const auto number = std::atoi(lower.c_str() + 1);
			if (number >= 1 && number <= 12) {
				return Key{ RE::INPUT_DEVICE::kKeyboard, 0x70 + number - 1 };
			}
		}
		if (lower.starts_with("mouse")) {
			const auto number = std::atoi(lower.c_str() + 5);
			if (lower.size() == 6 && std::isdigit(static_cast<unsigned char>(lower[5])) && number <= 7) {
				return Key{ RE::INPUT_DEVICE::kMouse, number };
			}
			return std::nullopt;
		}
		const auto hex = [](const char* a_text) -> std::optional<std::int32_t> {
			char*      end = nullptr;
			const auto value = std::strtol(a_text, &end, 16);
			return *end == '\0' && value > 0 ? std::make_optional(static_cast<std::int32_t>(value)) : std::nullopt;
		};
		if (lower.starts_with("pad:")) {
			const auto value = hex(lower.c_str() + 4);
			return value ? std::make_optional(Key{ RE::INPUT_DEVICE::kGamepad, *value }) : std::nullopt;
		}
		if (lower.starts_with("0x")) {
			const auto value = hex(lower.c_str());
			return value && *value < 0xFF ? std::make_optional(Key{ RE::INPUT_DEVICE::kKeyboard, *value }) : std::nullopt;
		}
		return std::nullopt;
	}
}

namespace Settings
{
	// Data\F4SE\Plugins\NightVision.ini ships with the mod; NightVisionCustom.ini, if present,
	// overrides it so user changes survive mod updates.
	inline constexpr auto FILE_BASE = "Data/F4SE/Plugins/NightVision.ini";
	inline constexpr auto FILE_USER = "Data/F4SE/Plugins/NightVisionCustom.ini";

	inline REX::TIniSetting<std::string> toggleKey{ "Keys", "sToggleKey", "N" };
	inline REX::TIniSetting<std::string> nightVisionKey{ "Keys", "sNightVisionKey", "" };
	inline REX::TIniSetting<std::string> thermalKey{ "Keys", "sThermalKey", "" };
	inline REX::TIniSetting<std::string> presetKey{ "Keys", "sPresetKey", "J" };
	inline REX::TIniSetting<std::string> assignKey{ "Keys", "sAssignKey", "K" };
	inline REX::TIniSetting<float>       holdSeconds{ "Keys", "fHoldSeconds", 0.5F };

	inline REX::TIniSetting<bool>  requireHelmet{ "General", "bRequireAssignedHelmet", true };
	inline REX::TIniSetting<bool>  autoFitPowerArmor{ "General", "bAutoFitPowerArmor", true };
	inline REX::TIniSetting<bool>  showMessages{ "General", "bShowMessages", true };
	inline REX::TIniSetting<float> transition{ "General", "fTransitionSeconds", 0.35F };
	inline REX::TIniSetting<bool>  nightVisionColors{ "General", "bNightVisionColors", true };
	inline REX::TIniSetting<bool>  thermalColors{ "General", "bThermalColors", true };

	inline REX::TIniSetting<std::string> thermalShader{ "Thermal", "sShader", "Fallout4.esm|22517C" };
	inline REX::TIniSetting<std::string> thermalHostileShader{ "Thermal", "sHostileShader", "Fallout4.esm|22517E" };
	inline REX::TIniSetting<float>       thermalRange{ "Thermal", "fRange", 20000.0F };
	inline REX::TIniSetting<bool>        thermalRobots{ "Thermal", "bShowRobots", false };
	inline REX::TIniSetting<bool>        thermalDead{ "Thermal", "bShowDead", false };

	inline REX::TIniSetting<std::string> overlayFile{ "Overlay", "sFile", "main" };
	inline REX::TIniSetting<std::string> overlayScreen{ "Overlay", "sScreenModel", "Interface/Objects/HUDGlassFlat.nif" };

	inline REX::TIniSetting<std::string> soundOn{ "Sound", "sOnFile", "Data/F4SE/Plugins/NightVision/nvg_on.mp3" };
	inline REX::TIniSetting<std::string> soundOff{ "Sound", "sOffFile", "Data/F4SE/Plugins/NightVision/nvg_off.mp3" };
	inline REX::TIniSetting<float>       soundVolume{ "Sound", "fVolume", 1.0F };
	inline REX::TIniSetting<std::string> soundPreset{ "Sound", "sPresetSound", "UIPipBoyRotaryHorizontalRight" };

	void Load()
	{
		auto* store = REX::FIniSettingStore::GetSingleton();
		store->Init(FILE_BASE, FILE_USER);
		store->Load();
	}
}

// MP3s need no plugin file or sound form this way. MCI devices belong to the thread that opened
// them and its calls can stall on disk, so one worker owns them and the game threads only queue.
// These play at their own volume, outside the game's audio sliders.
namespace Sound
{
	enum class Clip
	{
		kOn,
		kOff
	};

	inline std::mutex              lock;
	inline std::condition_variable wake;
	inline std::deque<Clip>        queue;

	bool Send(const std::string& a_command)
	{
		const auto error = mciSendStringA(a_command.c_str(), nullptr, 0, nullptr);
		if (error != 0) {
			char text[128]{};
			mciGetErrorStringA(error, text, sizeof(text));
			REX::ERROR("sound: {} failed: {}", a_command, text);
		}
		return error == 0;
	}

	[[nodiscard]] bool Open(const char* a_alias, const std::string& a_path, int a_volume)
	{
		if (a_path.empty()) {
			return false;
		}
		if (!Send(std::format("open \"{}\" type mpegvideo alias {}", a_path, a_alias))) {
			return false;
		}
		Send(std::format("setaudio {} volume to {}", a_alias, a_volume));
		REX::INFO("SUCCESS: sound {} ready", a_path);
		return true;
	}

	void Worker(std::string a_on, std::string a_off, int a_volume)
	{
		const auto hasOn = Open("hvOn", a_on, a_volume);
		const auto hasOff = Open("hvOff", a_off, a_volume);
		for (;;) {
			std::unique_lock guard{ lock };
			wake.wait(guard, [] { return !queue.empty(); });
			const auto clip = queue.front();
			queue.pop_front();
			guard.unlock();

			if (clip == Clip::kOn && hasOn) {
				Send("stop hvOff");
				Send("play hvOn from 0");
			} else if (clip == Clip::kOff && hasOff) {
				Send("stop hvOn");
				Send("play hvOff from 0");
			}
		}
	}

	void Start()
	{
		const auto volume = std::clamp(static_cast<int>(Settings::soundVolume.GetValue() * 1000.0F), 0, 1000);
		std::thread{ Worker, Settings::soundOn.GetValue(), Settings::soundOff.GetValue(), volume }.detach();
	}

	void Play(Clip a_clip)
	{
		{
			std::scoped_lock guard{ lock };
			queue.push_back(a_clip);
		}
		wake.notify_one();
	}
}

namespace Presets
{
	enum class Mode : std::uint8_t
	{
		kNightVision = 0,
		kThermal = 1
	};

	[[nodiscard]] constexpr const char* ModeName(Mode a_mode) noexcept
	{
		return a_mode == Mode::kThermal ? "Thermal" : "Night vision";
	}

	struct Preset
	{
		std::string name;
		float       red{ 1.0F };
		float       green{ 1.0F };
		float       blue{ 1.0F };
		float       tintAmount{ 0.9F };
		float       saturation{ 0.0F };
		float       brightness{ 1.0F };
		float       contrast{ 1.0F };
		float       bloomScale{ -1.0F };        // below 0 keeps the game's value
		float       eyeAdaptStrength{ -1.0F };  // below 0 keeps the game's value
		float       motionBlur{ -1.0F };        // below 0 keeps the game's value
		float       motionBlurMax{ -1.0F };     // below 0 keeps the game's value
		float       dofStrength{ -1.0F };       // below 0 keeps the game's value
		float       dofDistance{ 1500.0F };
		float       dofRange{ 4000.0F };
		float       filmGrain{ 0.0F };
	};

	inline std::array<std::vector<Preset>, 2> lists;

	[[nodiscard]] const std::vector<Preset>& List(Mode a_mode)
	{
		return lists[std::to_underlying(a_mode)];
	}

	[[nodiscard]] const Preset& Find(Mode a_mode, std::string_view a_name)
	{
		const auto& list = List(a_mode);
		const auto  it = std::ranges::find(list, a_name, &Preset::name);
		return it != list.end() ? *it : list.front();
	}

	[[nodiscard]] const Preset& Next(Mode a_mode, std::string_view a_name)
	{
		const auto& list = List(a_mode);
		const auto  it = std::ranges::find(list, a_name, &Preset::name);
		return it == list.end() || std::next(it) == list.end() ? list.front() : *std::next(it);
	}

	void Parse(const CSimpleIniA& a_file, const char* a_section, Preset& a_preset)
	{
		const auto number = [&](const char* a_key, float a_fallback) {
			return static_cast<float>(a_file.GetDoubleValue(a_section, a_key, a_fallback));
		};
		if (const auto* color = a_file.GetValue(a_section, "sColor", nullptr)) {
			float red = 0.0F;
			float green = 0.0F;
			float blue = 0.0F;
			if (sscanf_s(color, "%f , %f , %f", &red, &green, &blue) == 3) {
				a_preset.red = red;
				a_preset.green = green;
				a_preset.blue = blue;
			} else {
				REX::WARN("[{}] sColor = '{}' is not three numbers", a_section, color);
			}
		}
		a_preset.tintAmount = number("fTintAmount", a_preset.tintAmount);
		a_preset.saturation = number("fSaturation", a_preset.saturation);
		a_preset.brightness = number("fBrightness", a_preset.brightness);
		a_preset.contrast = number("fContrast", a_preset.contrast);
		a_preset.bloomScale = number("fBloomScale", a_preset.bloomScale);
		a_preset.eyeAdaptStrength = number("fEyeAdaptStrength", a_preset.eyeAdaptStrength);
		a_preset.motionBlur = number("fMotionBlur", a_preset.motionBlur);
		a_preset.motionBlurMax = number("fMotionBlurMax", a_preset.motionBlurMax);
		a_preset.dofStrength = number("fDofStrength", a_preset.dofStrength);
		a_preset.dofDistance = number("fDofDistance", a_preset.dofDistance);
		a_preset.dofRange = number("fDofRange", a_preset.dofRange);
		a_preset.filmGrain = number("fFilmGrain", a_preset.filmGrain);
	}

	// Sections are [NightVision.Name] and [Thermal.Name], in file order. A section in the user file
	// with an existing name changes that preset, and a new name adds one.
	void Load()
	{
		for (const auto* path : { Settings::FILE_BASE, Settings::FILE_USER }) {
			CSimpleIniA file;
			file.SetUnicode(true);
			if (file.LoadFile(path) < 0) {
				continue;
			}
			CSimpleIniA::TNamesDepend sections;
			file.GetAllSections(sections);
			sections.sort(CSimpleIniA::Entry::LoadOrder());
			for (const auto& section : sections) {
				const std::string_view name{ section.pItem };
				const auto             dot = name.find('.');
				if (dot == std::string_view::npos || dot + 1 == name.size()) {
					continue;
				}
				const auto group = Keys::Lower(name.substr(0, dot));
				if (group != "nightvision" && group != "thermal") {
					continue;
				}
				auto&      list = lists[group == "thermal" ? 1 : 0];
				const auto label = name.substr(dot + 1);
				auto       it = std::ranges::find(list, label, &Preset::name);
				if (it == list.end()) {
					it = list.insert(list.end(), Preset{ .name = std::string{ label } });
				}
				Parse(file, section.pItem, *it);
			}
		}

		if (lists[0].empty()) {
			lists[0].push_back({ .name = "Green", .red = 0.25F, .green = 1.0F, .blue = 0.3F, .brightness = 1.5F, .contrast = 1.15F });
		}
		if (lists[1].empty()) {
			lists[1].push_back({ .name = "White Hot", .red = 0.9F, .green = 0.92F, .blue = 1.0F, .brightness = 0.55F, .contrast = 1.5F });
		}
		REX::INFO("presets: {} night vision, {} thermal", lists[0].size(), lists[1].size());
	}
}

namespace Vision
{
	using Presets::Mode;

	struct Helmet
	{
		Mode        mode{ Mode::kNightVision };
		std::string nightVisionPreset;
		std::string thermalPreset;
		std::string overlay;  // unused, kept so saves from 0.1 still read

		[[nodiscard]] std::string& Preset(Mode a_mode) { return a_mode == Mode::kThermal ? thermalPreset : nightVisionPreset; }
	};

	// Input arrives on several job threads, while the frame hook and co-save callbacks run elsewhere,
	// so everything that touches this state holds stateLock.
	inline std::recursive_mutex            stateLock;
	inline std::map<RE::TESFormID, Helmet> helmets;
	inline Helmet                          loose;  // settings used when bRequireAssignedHelmet is off
	inline bool                            active{ false };
	inline bool                            restorePending{ false };
	inline RE::TESFormID                   activeHelmet{ 0 };
	inline Presets::Preset                 current;
	inline Mode                            currentMode{ Mode::kNightVision };
	inline std::string                     currentOverlay;  // file name without folder, empty for none
	inline std::string                     shownOverlay;    // what the open menu is showing
	inline float                           shownGrain{ 0.0F };
	inline bool                            overlayOpen{ false };
	inline float                           blend{ 0.0F };
	inline float                           wornTimer{ 0.0F };
	inline float                           scanTimer{ 0.0F };
	inline std::chrono::steady_clock::time_point lastTick{};

	struct Glow
	{
		float          age;
		RE::TESEffectShader* shader;
	};

	struct Wanted
	{
		RE::NiPointer<RE::Actor> actor;
		RE::TESEffectShader*     shader{ nullptr };
	};

	inline constexpr float GLOW_SECONDS = 2.0F;
	inline constexpr float SCAN_SECONDS = 0.5F;
	inline constexpr float WORN_SECONDS = 0.25F;

	inline std::unordered_map<std::uint32_t, Glow> glows;
	inline RE::TESEffectShader*                    glowShader{ nullptr };
	inline RE::TESEffectShader*                    glowHostileShader{ nullptr };
	inline RE::BGSKeyword*                         robotKeyword{ nullptr };
	inline RE::BGSKeyword*                         turretKeyword{ nullptr };

	void Notify(const std::string& a_text)
	{
		REX::INFO("{}", a_text);
		if (Settings::showMessages.GetValue()) {
			RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), "", false, false);
		}
	}

	// Helmets sit on the hair and headband slots; goggles and masks on the eyes and mouth.
	inline constexpr std::array HEAD_SLOTS{
		RE::BIPED_OBJECT::kHairTop, RE::BIPED_OBJECT::kHeadband, RE::BIPED_OBJECT::kHairLong,
		RE::BIPED_OBJECT::kEyes, RE::BIPED_OBJECT::kMouth
	};

	[[nodiscard]] std::vector<RE::TESObjectARMO*> WornHeadgear()
	{
		std::vector<RE::TESObjectARMO*> worn;
		const auto*                     player = RE::PlayerCharacter::GetSingleton();
		const auto*                     biped = player ? player->biped.get() : nullptr;
		if (!biped) {
			return worn;
		}
		for (const auto slot : HEAD_SLOTS) {
			auto* form = biped->GetBipObject(slot)->parent.object;
			auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr;
			if (armor && std::ranges::find(worn, armor) == worn.end()) {
				worn.push_back(armor);
			}
		}
		return worn;
	}

	// The worn piece that has vision assigned to it.
	[[nodiscard]] RE::TESObjectARMO* WornAssigned()
	{
		for (auto* armor : WornHeadgear()) {
			if (helmets.contains(armor->GetFormID())) {
				return armor;
			}
		}
		return nullptr;
	}

	[[nodiscard]] Helmet* Settings()
	{
		if (const auto* armor = WornAssigned()) {
			return std::addressof(helmets[armor->GetFormID()]);
		}
		if (Settings::autoFitPowerArmor.GetValue() && RE::PowerArmor::PlayerInPowerArmor()) {
			if (const auto worn = WornHeadgear(); !worn.empty()) {
				return std::addressof(helmets[worn.front()->GetFormID()]);
			}
		}
		return Settings::requireHelmet.GetValue() ? nullptr : std::addressof(loose);
	}

	[[nodiscard]] std::string Describe(Mode a_mode, const Presets::Preset& a_preset)
	{
		return std::format("{} ({})", Presets::ModeName(a_mode), a_preset.name);
	}

	void Select(Helmet& a_helmet)
	{
		currentMode = a_helmet.mode;
		current = Presets::Find(a_helmet.mode, a_helmet.Preset(a_helmet.mode));
		a_helmet.Preset(a_helmet.mode) = current.name;
		const auto overlay = Settings::overlayFile.GetValue();
		currentOverlay = Keys::Lower(overlay) == "none" ? std::string{} : overlay;
	}

	bool Activate(Helmet& a_helmet, bool a_silent = false)
	{
		if (!active && !a_silent) {
			Sound::Play(Sound::Clip::kOn);
		}
		Select(a_helmet);
		const auto* armor = WornAssigned();
		activeHelmet = armor ? armor->GetFormID() : 0;
		active = true;
		wornTimer = 0.0F;
		scanTimer = SCAN_SECONDS;
		REX::INFO("SUCCESS: activated {} on helmet {:08X}", Describe(currentMode, current), activeHelmet);
		return true;
	}

	void Deactivate()
	{
		Sound::Play(Sound::Clip::kOff);
		active = false;
		activeHelmet = 0;
		REX::INFO("SUCCESS: vision off");
	}

	void NoHelmetMessage()
	{
		Notify(WornHeadgear().empty() ? "Night vision: no helmet worn" : "Night vision: hold the assign key to fit this helmet");
	}

	void Toggle()
	{
		if (active) {
			Deactivate();
			Notify("Vision off");
			return;
		}
		auto* helmet = Settings();
		if (!helmet) {
			NoHelmetMessage();
			return;
		}
		Activate(*helmet);
		Notify(Describe(currentMode, current));
	}

	void SwitchTo(Mode a_mode)
	{
		auto* helmet = Settings();
		if (!helmet) {
			NoHelmetMessage();
			return;
		}
		if (active && currentMode == a_mode) {
			Toggle();
			return;
		}
		helmet->mode = a_mode;
		Activate(*helmet);
		Notify(Describe(currentMode, current));
	}

	void CycleMode()
	{
		auto* helmet = Settings();
		if (!helmet) {
			NoHelmetMessage();
			return;
		}
		helmet->mode = helmet->mode == Mode::kThermal ? Mode::kNightVision : Mode::kThermal;
		Select(*helmet);
		if (active) {
			Activate(*helmet);
		}
		Notify(Describe(currentMode, current));
	}

	void PlayClick();

	void NextPreset()
	{
		auto* helmet = Settings();
		if (!helmet) {
			NoHelmetMessage();
			return;
		}
		const auto& next = Presets::Next(helmet->mode, helmet->Preset(helmet->mode));
		helmet->Preset(helmet->mode) = next.name;
		Select(*helmet);
		PlayClick();
		Notify(Describe(currentMode, current));
	}

	void PlayClick()
	{
		if (const auto sound = Settings::soundPreset.GetValue(); !sound.empty()) {
			RE::UIUtils::PlayMenuSound(sound.c_str());
		}
	}

	void AssignToggle()
	{
		const auto worn = WornHeadgear();
		if (worn.empty()) {
			Notify("Night vision: no helmet worn");
			return;
		}
		auto*       armor = worn.front();
		const auto  id = armor->GetFormID();
		const auto* name = armor->GetFullName();
		const auto  label = name && name[0] != '\0' ? std::string{ name } : std::format("{:08X}", id);

		if (const auto* assigned = WornAssigned()) {
			const auto assignedID = assigned->GetFormID();
			const auto* assignedName = assigned->GetFullName();
			if (active && activeHelmet == assignedID) {
				Deactivate();
			}
			helmets.erase(assignedID);
			REX::INFO("SUCCESS: removed vision from helmet {:08X}", assignedID);
			Notify(std::format("Vision removed: {}", assignedName ? assignedName : ""));
			return;
		}

		auto& helmet = helmets[id];
		Select(helmet);
		REX::INFO("SUCCESS: assigned vision to helmet {:08X} {}", id, label);
		Notify(std::format("Vision fitted: {}", label));
	}

	template <class T>
	[[nodiscard]] T* LookupForm(const std::string& a_text)
	{
		const auto split = a_text.find('|');
		auto*      handler = RE::TESDataHandler::GetSingleton();
		if (split == std::string::npos || !handler) {
			return nullptr;
		}
		const auto id = std::strtoul(a_text.c_str() + split + 1, nullptr, 16);
		return handler->LookupForm<T>(static_cast<RE::TESFormID>(id), Keys::Trim(std::string_view{ a_text }.substr(0, split)));
	}

	void LoadForms()
	{
		glowShader = LookupForm<RE::TESEffectShader>(Settings::thermalShader.GetValue());
		glowHostileShader = LookupForm<RE::TESEffectShader>(Settings::thermalHostileShader.GetValue());
		if (!glowHostileShader) {
			glowHostileShader = glowShader;
		}
		robotKeyword = LookupForm<RE::BGSKeyword>("Fallout4.esm|2CB73");
		turretKeyword = LookupForm<RE::BGSKeyword>("Fallout4.esm|B2BF3");
		if (glowShader) {
			REX::INFO("SUCCESS: thermal shader {:08X}, hostile shader {:08X}", glowShader->GetFormID(), glowHostileShader->GetFormID());
		} else {
			REX::ERROR("thermal shader '{}' not found; thermal mode will have no actor glow", Settings::thermalShader.GetValue());
		}
	}

	[[nodiscard]] bool ShowsHeat(RE::Actor& a_actor)
	{
		if (a_actor.IsDisabled() || !a_actor.Get3D() || (!Settings::thermalDead.GetValue() && a_actor.IsDead(true))) {
			return false;
		}
		if (Settings::thermalRobots.GetValue() || !a_actor.race) {
			return true;
		}
		return !((robotKeyword && a_actor.race->HasKeyword(robotKeyword)) || (turretKeyword && a_actor.race->HasKeyword(turretKeyword)));
	}

	// a_on false ends every glow this plugin started.
	void UpdateGlows(bool a_on, float a_delta)
	{
		auto* lists = RE::ProcessLists::GetSingleton();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!lists || !player || (!a_on && glows.empty())) {
			return;
		}

		std::unordered_map<std::uint32_t, Wanted> wanted;
		if (a_on && glowShader) {
			const auto range = Settings::thermalRange.GetValue();
			const auto rangeSq = range * range;
			const auto& origin = player->data.location;
			// High process only covers actors close to the player. ShowsHeat skips any without 3D.
			for (const auto* group : { &lists->highActorHandles, &lists->middleHighActorHandles, &lists->middleLowActorHandles, &lists->lowActorHandles }) {
				for (auto handle : *group) {
					const auto actor = handle.get();
					if (!actor || actor.get() == player || !ShowsHeat(*actor)) {
						continue;
					}
					const auto& location = actor->data.location;
					const auto  x = location.x - origin.x;
					const auto  y = location.y - origin.y;
					const auto  z = location.z - origin.z;
					if (x * x + y * y + z * z <= rangeSq) {
						wanted[handle.get_handle()] = { actor, actor->GetHostileToActor(player) ? glowHostileShader : glowShader };
					}
				}
			}
		}

		lists->ForEachShaderEffect([&](RE::ShaderReferenceEffect* a_effect) {
			auto       target = a_effect->target;
			const auto handle = target.get_handle();
			const auto glow = glows.find(handle);
			if (glow == glows.end() || a_effect->effectData != glow->second.shader) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto want = wanted.find(handle);
			if (want != wanted.end() && want->second.shader == a_effect->effectData) {
				a_effect->lifetime = a_effect->age + GLOW_SECONDS;
				glow->second.age = 0.0F;
			} else {
				a_effect->lifetime = a_effect->age;  // the game removes it on its next update
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});

		std::erase_if(glows, [&](auto& a_entry) {
			auto& [handle, glow] = a_entry;
			glow.age += a_delta;
			const auto want = wanted.find(handle);
			return want == wanted.end() || want->second.shader != glow.shader || glow.age >= GLOW_SECONDS;
		});

		for (const auto& [handle, want] : wanted) {
			if (!glows.contains(handle) && want.actor->ApplyEffectShader(want.shader, GLOW_SECONDS)) {
				glows[handle] = { 0.0F, want.shader };
			}
		}
	}

	[[nodiscard]] constexpr float Mix(float a_from, float a_to, float a_amount) noexcept
	{
		return a_from + (a_to - a_from) * a_amount;
	}

	void OverlayLight(RE::ImageSpaceBaseData& a_data)
	{
		if (current.bloomScale >= 0.0F) {
			a_data.hdrData.bloomScale = Mix(a_data.hdrData.bloomScale, current.bloomScale, blend);
		}
		if (current.eyeAdaptStrength >= 0.0F) {
			a_data.hdrData.eyeAdaptStrength = Mix(a_data.hdrData.eyeAdaptStrength, current.eyeAdaptStrength, blend);
		}
		// The finish step copies these into the frame unless an imagespace modifier set its own.
		if (current.dofStrength >= 0.0F) {
			a_data.dofData.strength = Mix(a_data.dofData.strength, current.dofStrength, blend);
			a_data.dofData.distance = Mix(a_data.dofData.distance, current.dofDistance, blend);
			a_data.dofData.range = Mix(a_data.dofData.range, current.dofRange, blend);
		}
	}

	void Overlay(RE::ImageSpaceData& a_eof)
	{
		auto& data = a_eof.baseData;
		auto& cinematic = data.cinematicData;
		cinematic.brightness = Mix(cinematic.brightness, current.brightness, blend);
		cinematic.contrast = Mix(cinematic.contrast, current.contrast, blend);

		const auto colors = currentMode == Presets::Mode::kThermal ? Settings::thermalColors.GetValue() : Settings::nightVisionColors.GetValue();
		if (!colors) {
			OverlayLight(data);
			return;
		}

		cinematic.saturation = Mix(cinematic.saturation, current.saturation, blend);

		auto& tint = data.tintData;
		// At this point the copy is premultiplied: the colour holds colour * amount, so modifiers can
		// add to it. The finish step divides by the amount sum, then uses highestTintAmount as the
		// final amount, so that field has to rise with the tint or the picture stays grey.
		tint.color.red = Mix(tint.color.red, current.red * current.tintAmount, blend);
		tint.color.green = Mix(tint.color.green, current.green * current.tintAmount, blend);
		tint.color.blue = Mix(tint.color.blue, current.blue * current.tintAmount, blend);
		tint.amount = Mix(tint.amount, current.tintAmount, blend);
		a_eof.highestTintAmount = Mix(a_eof.highestTintAmount, current.tintAmount, blend);

		OverlayLight(data);
	}

	void UpdateOverlay(bool a_wanted);

	void Shutdown()
	{
		UpdateOverlay(false);
		active = false;
		activeHelmet = 0;
		blend = 0.0F;
		glows.clear();
	}

	void RestoreAfterLoad()
	{
		restorePending = false;
		if (auto* helmet = Settings()) {
			Activate(*helmet, true);
		} else {
			REX::INFO("vision was on in this save, but no fitted helmet is worn");
		}
	}

	void Tick(RE::ImageSpaceManager& a_manager)
	{
		if (!active && !restorePending && !overlayOpen && blend <= 0.0F && glows.empty()) {
			return;
		}

		std::scoped_lock guard{ stateLock };
		const auto now = std::chrono::steady_clock::now();
		const auto delta = (std::min)(std::chrono::duration<float>(now - lastTick).count(), 0.25F);
		lastTick = now;

		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* ui = RE::UI::GetSingleton();
		if (!player || !player->parentCell || (ui && ui->GetMenuOpen("MainMenu"sv))) {
			Shutdown();
			return;
		}

		if (restorePending) {
			if (!player->Get3D()) {
				return;
			}
			RestoreAfterLoad();
		}

		if (active && activeHelmet != 0) {
			wornTimer += delta;
			if (wornTimer >= WORN_SECONDS) {
				wornTimer = 0.0F;
				const auto* armor = WornAssigned();
				if (!armor) {
					Deactivate();
					Notify("Vision off: helmet removed");
				} else {
					activeHelmet = armor->GetFormID();
				}
			}
		}

		const auto seconds = Settings::transition.GetValue();
		const auto step = seconds > 0.0F ? delta / seconds : 1.0F;
		blend = std::clamp(blend + (active ? step : -step), 0.0F, 1.0F);
		if (blend > 0.0F) {
			Overlay(a_manager.currentEOFData);
		}

		const auto* camera = RE::PlayerCamera::GetSingleton();
		const auto  firstPerson = camera && camera->QCameraEquals(RE::CameraState::kFirstPerson);
		UpdateOverlay(active && firstPerson && ui && ui->menuMode == 0 && !currentOverlay.empty());

		const auto thermal = active && currentMode == Mode::kThermal;
		if (thermal ? ui && ui->menuMode == 0 : !glows.empty()) {
			scanTimer += delta;
			if (scanTimer >= SCAN_SECONDS || !thermal) {
				UpdateGlows(thermal, scanTimer);
				scanTimer = 0.0F;
			}
		}
	}
}

namespace Vision
{
	// Plain menus are not drawn over gameplay, so this sets up its own screen the way the ScopeMenu
	// constructor does: a custom renderer, a flat screen model 350 units in front of the camera, and
	// the HUDGlassFlat material. Alpha blending lets the black mask cover the world.
	[[nodiscard]] RE::Interface3D::Renderer* OverlayRenderer(const RE::BSFixedString& a_name)
	{
		using Renderer = RE::Interface3D::Renderer;
		if (auto* existing = Renderer::GetByName(a_name)) {
			return existing;
		}
		auto* renderer = Renderer::Create(a_name, RE::UI_DEPTH_PRIORITY::kScope, 0.0F, false);
		if (!renderer) {
			REX::ERROR("could not create the overlay renderer");
			return nullptr;
		}

		RE::BSModelDB::DBTraits::ArgsType args{};
		args.prepareAfterLoad = 1;
		args.useErrorMarker = 1;
		args.performProcess = 1;
		args.createFadeNode = 1;
		args.loadTextures = 1;
		RE::NiPointer<RE::NiNode> root;
		const auto                model = Settings::overlayScreen.GetValue();
		RE::BSModelDB::Demand(model.c_str(), std::addressof(root), args);
		if (!root) {
			REX::ERROR("could not load overlay screen model {}", model);
			return nullptr;
		}
		root->local.translate = { 0.0F, 350.0F, 0.0F };
		RE::NiUpdateData update;
		root->Update(update);

		renderer->MainScreen_SetScreenAttached3D(root.get());
		renderer->MainScreen_SetPostAA(true);
		renderer->MainScreen_SetBackgroundMode(RE::Interface3D::BackgroundMode::kLive);
		renderer->MainScreen_SetMenuBlendMode(RE::Interface3D::OffscreenMenuBlendMode::kAlpha);
		renderer->Offscreen_SetRenderTargetSize(RE::Interface3D::OffscreenMenuSize::kFullFrame);
		renderer->Offscreen_SetDisplayMode(RE::Interface3D::ScreenMode::kScreenAttached, "", "Materials\\Interface\\HUDGlassFlat.BGEM");
		REX::INFO("SUCCESS: overlay renderer ready with {}", model);
		return renderer;
	}

	// A menu with no input that sits under the HUD. It lives only while an overlay is showing.
	class OverlayMenu final : public RE::IMenu
	{
	public:
		static constexpr auto MENU_NAME{ "NightVisionOverlayMenu"sv };

		OverlayMenu()
		{
			menuFlags.set(RE::UI_MENU_FLAGS::kAllowSaving, RE::UI_MENU_FLAGS::kDontHideCursorWhenTopmost, RE::UI_MENU_FLAGS::kCustomRendering);
			depthPriority = RE::UI_DEPTH_PRIORITY::kScope;
			customRendererName = MENU_NAME;
			const auto path = std::format("Interface/NightVision/{}.swf", shownOverlay);
			auto*      manager = RE::BSScaleformManager::GetSingleton();
			if (manager && manager->LoadMovieEx(*this, path, ""sv, RE::BSScaleformManager::ScaleModeType::kExactFit)) {
				const auto* definition = uiMovie ? uiMovie->GetMovieDef() : nullptr;
				REX::INFO("SUCCESS: overlay {} showing ({}x{}, {} frames, flash {})", path,
					definition ? definition->GetWidth() : 0.0F, definition ? definition->GetHeight() : 0.0F,
					definition ? definition->GetFrameCount() : 0, definition ? definition->GetVersion() : 0);
			} else {
				REX::ERROR("could not load overlay {}", path);
			}
			SetGrain();
			renderer = OverlayRenderer(customRendererName);
			if (renderer) {
				renderer->Enable(false);
			}
		}

		~OverlayMenu() override
		{
			if (renderer) {
				renderer->Disable();
			}
		}

		static RE::IMenu* Create(const RE::UIMessage&) { return new OverlayMenu(); }

	private:
		void SetGrain()
		{
			Scaleform::GFx::Value root;
			Scaleform::GFx::Value grain;
			Scaleform::GFx::Value name{ "grain" };
			if (!uiMovie || !uiMovie->GetVariable(std::addressof(root), "root") || !root.Invoke("getChildByName", std::addressof(grain), std::addressof(name), 1) || !grain.IsObject()) {
				REX::WARN("overlay has no grain layer");
				return;
			}
			grain.SetMember("alpha", static_cast<double>(shownGrain));
			grain.SetMember("visible", shownGrain > 0.0F);
		}

		RE::Interface3D::Renderer* renderer{ nullptr };
	};

	void UpdateOverlay(bool a_wanted)
	{
		auto* queue = RE::UIMessageQueue::GetSingleton();
		if (!queue) {
			return;
		}
		// A different file needs a new menu, so the old one closes here and the next frame reopens it.
		if (overlayOpen && (!a_wanted || shownOverlay != currentOverlay || shownGrain != current.filmGrain)) {
			queue->AddMessage(OverlayMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide);
			overlayOpen = false;
		} else if (!overlayOpen && a_wanted) {
			const auto* ui = RE::UI::GetSingleton();
			if (ui && !ui->GetMenuOpen(OverlayMenu::MENU_NAME)) {
				shownOverlay = currentOverlay;
				shownGrain = current.filmGrain;
				queue->AddMessage(OverlayMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow);
				overlayOpen = true;
			}
		}
	}

	void RegisterOverlay()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->RegisterMenu(OverlayMenu::MENU_NAME.data(), OverlayMenu::Create);
			REX::INFO("SUCCESS: overlay menu registered");
		} else {
			REX::ERROR("UI is not available, so overlays are off");
		}
	}
}

namespace Hooks
{
	// Main::OnIdle_UpdateImageSpace, ImageSpaceManager::ResetToBaseData
	inline const REL::VariantID UPDATE_IMAGE_SPACE{ 1169995, 2228932 };
	inline const REL::VariantID RESET_TO_BASE_DATA{ 1438195, 2316610 };
	inline constexpr std::size_t SEARCH_WINDOW = 0x100;

	struct ResetToBaseData
	{
		static void thunk(RE::ImageSpaceManager* a_this)
		{
			// The caller sets this from the game's setting every frame, and the reset copies it.
			if (Vision::blend > 0.0F) {
				if (Vision::current.motionBlur >= 0.0F) {
					a_this->motionBlurIntensity = Vision::Mix(a_this->motionBlurIntensity, Vision::current.motionBlur, Vision::blend);
				}
				if (Vision::current.motionBlurMax >= 0.0F) {
					a_this->motionBlurMaxBlur = Vision::Mix(a_this->motionBlurMaxBlur, Vision::current.motionBlurMax, Vision::blend);
				}
			}
			func(a_this);
			Vision::Tick(*a_this);
		}

		static inline REL::Relocation<void (*)(RE::ImageSpaceManager*)> func;
	};

	[[nodiscard]] bool Install()
	{
		// Only the one call in this function is hooked, because other callers reset the data for
		// menus. The call is found by its target so a build with moved code is reported, not patched.
		const auto function = UPDATE_IMAGE_SPACE.address();
		const auto target = RESET_TO_BASE_DATA.address();
		const std::span code{ reinterpret_cast<const std::uint8_t*>(function), SEARCH_WINDOW };

		std::optional<std::uintptr_t> site;
		for (std::size_t i = 0; i + 5 <= code.size(); ++i) {
			if (code[i] != 0xE8) {
				continue;
			}
			std::int32_t displacement = 0;
			std::memcpy(&displacement, code.data() + i + 1, sizeof(displacement));
			if (function + i + 5 + displacement == target) {
				if (site) {
					REX::ERROR("more than one ResetToBaseData call at offset {:X}", UPDATE_IMAGE_SPACE.offset());
					return false;
				}
				site = function + i;
			}
		}
		if (!site) {
			REX::ERROR("ResetToBaseData call not found at offset {:X}", UPDATE_IMAGE_SPACE.offset());
			return false;
		}

		ResetToBaseData::func = REL::GetTrampoline().write_call<5>(*site, &ResetToBaseData::thunk);
		REX::INFO("SUCCESS: frame hook installed at offset {:X}", UPDATE_IMAGE_SPACE.offset() + (*site - function));
		return true;
	}
}

namespace Input
{
	using Action = void (*)();

	struct Binding
	{
		Keys::Key   key;
		const char* name;
		Action      tap;
		Action      hold;
		bool        held{ false };
	};

	inline std::vector<Binding> bindings;

	void Add(const char* a_name, const std::string& a_text, Action a_tap, Action a_hold)
	{
		const auto key = Keys::Parse(a_text);
		if (!key) {
			REX::ERROR("{} = '{}' is not a key", a_name, a_text);
			return;
		}
		if (key->IsSet()) {
			bindings.push_back({ *key, a_name, a_tap, a_hold });
		}
	}

	void Load()
	{
		Add("sToggleKey", Settings::toggleKey.GetValue(), Vision::Toggle, Vision::CycleMode);
		Add("sNightVisionKey", Settings::nightVisionKey.GetValue(), [] { Vision::SwitchTo(Presets::Mode::kNightVision); }, nullptr);
		Add("sThermalKey", Settings::thermalKey.GetValue(), [] { Vision::SwitchTo(Presets::Mode::kThermal); }, nullptr);
		Add("sPresetKey", Settings::presetKey.GetValue(), Vision::NextPreset, nullptr);
		Add("sAssignKey", Settings::assignKey.GetValue(), nullptr, Vision::AssignToggle);

		for (std::size_t i = 0; i < bindings.size(); ++i) {
			for (std::size_t j = i + 1; j < bindings.size(); ++j) {
				if (bindings[i].key == bindings[j].key) {
					REX::WARN("{} and {} share a key, so both will run", bindings[i].name, bindings[j].name);
				}
			}
		}
	}

	[[nodiscard]] bool InGameplay()
	{
		const auto* ui = RE::UI::GetSingleton();
		const auto* map = RE::ControlMap::GetSingleton();
		return ui && map && ui->menuMode == 0 && map->byTextEntryCount == 0;
	}

	void Handle(const RE::ButtonEvent& a_event)
	{
		for (auto& binding : bindings) {
			if (!binding.key.Matches(a_event)) {
				continue;
			}
			std::scoped_lock guard{ Vision::stateLock };
			if (a_event.QPressed()) {
				if (!binding.held && binding.hold && a_event.heldDownSecs >= Settings::holdSeconds.GetValue()) {
					binding.held = true;
					binding.hold();
				}
				continue;
			}
			if (!binding.held && binding.tap) {
				binding.tap();
			}
			binding.held = false;
		}
	}

	struct Receiver
	{
		static void thunk(RE::BSInputEventReceiver* a_this, const RE::InputEvent* a_queue)
		{
			func(a_this, a_queue);
			if (bindings.empty() || !InGameplay()) {
				return;
			}
			for (auto* event = const_cast<RE::InputEvent*>(a_queue); event; event = event->next) {
				if (event->eventType != RE::INPUT_EVENT_TYPE::kButton) {
					continue;
				}
				const auto* button = static_cast<RE::ButtonEvent*>(event);
				if (!button->disabled) {
					Handle(*button);
				}
			}
		}

		static inline REL::Relocation<void (*)(RE::BSInputEventReceiver*, const RE::InputEvent*)> func;
	};

	void Install()
	{
		// PerformInputProcessing is the only virtual of BSInputEventReceiver.
		REL::Relocation<std::uintptr_t> menuControls{ RE::VTABLE::MenuControls[0] };
		Receiver::func = menuControls.write_vfunc(0, &Receiver::thunk);
		REX::INFO("SUCCESS: input hook installed with {} keys", bindings.size());
	}
}

namespace Save
{
	inline constexpr std::uint32_t UNIQUE_ID = 'HVIS';
	inline constexpr std::uint32_t RECORD_HELMETS = 'HELM';
	inline constexpr std::uint32_t RECORD_STATE = 'STAT';
	inline constexpr std::uint32_t VERSION = 2;  // 2 added the overlay name

	void WriteText(const F4SE::SerializationInterface* a_intfc, const std::string& a_text)
	{
		const auto size = static_cast<std::uint32_t>(a_text.size());
		a_intfc->WriteRecordData(size);
		a_intfc->WriteRecordData(a_text.data(), size);
	}

	[[nodiscard]] std::string ReadText(const F4SE::SerializationInterface* a_intfc)
	{
		std::uint32_t size = 0;
		a_intfc->ReadRecordData(size);
		std::string text(size, '\0');
		a_intfc->ReadRecordData(text.data(), size);
		return text;
	}

	void WriteHelmet(const F4SE::SerializationInterface* a_intfc, const Vision::Helmet& a_helmet)
	{
		a_intfc->WriteRecordData(std::to_underlying(a_helmet.mode));
		WriteText(a_intfc, a_helmet.nightVisionPreset);
		WriteText(a_intfc, a_helmet.thermalPreset);
		WriteText(a_intfc, a_helmet.overlay);
	}

	[[nodiscard]] Vision::Helmet ReadHelmet(const F4SE::SerializationInterface* a_intfc, std::uint32_t a_version)
	{
		std::uint8_t mode = 0;
		a_intfc->ReadRecordData(mode);
		Vision::Helmet helmet;
		helmet.mode = mode == 1 ? Presets::Mode::kThermal : Presets::Mode::kNightVision;
		helmet.nightVisionPreset = ReadText(a_intfc);
		helmet.thermalPreset = ReadText(a_intfc);
		if (a_version >= 2) {
			helmet.overlay = ReadText(a_intfc);
		}
		return helmet;
	}

	void OnSave(const F4SE::SerializationInterface* a_intfc)
	{
		std::scoped_lock guard{ Vision::stateLock };
		if (a_intfc->OpenRecord(RECORD_HELMETS, VERSION)) {
			a_intfc->WriteRecordData(static_cast<std::uint32_t>(Vision::helmets.size()));
			for (const auto& [id, helmet] : Vision::helmets) {
				a_intfc->WriteRecordData(id);
				WriteHelmet(a_intfc, helmet);
			}
		} else {
			REX::ERROR("could not open the helmet record");
		}

		if (a_intfc->OpenRecord(RECORD_STATE, VERSION)) {
			const std::uint8_t active = Vision::active ? 1 : 0;
			a_intfc->WriteRecordData(active);
			WriteHelmet(a_intfc, Vision::loose);
		} else {
			REX::ERROR("could not open the state record");
		}
		REX::INFO("SUCCESS: saved {} helmets", Vision::helmets.size());
	}

	void OnLoad(const F4SE::SerializationInterface* a_intfc)
	{
		std::scoped_lock guard{ Vision::stateLock };
		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (version == 0 || version > VERSION) {
				REX::WARN("skipping co-save record of version {}", version);
				continue;
			}
			if (type == RECORD_HELMETS) {
				std::uint32_t count = 0;
				a_intfc->ReadRecordData(count);
				for (std::uint32_t i = 0; i < count; ++i) {
					RE::TESFormID id = 0;
					a_intfc->ReadRecordData(id);
					auto helmet = ReadHelmet(a_intfc, version);
					const auto resolved = a_intfc->ResolveFormID(id);
					if (resolved) {
						Vision::helmets[*resolved] = std::move(helmet);
					} else {
						REX::WARN("helmet {:08X} is no longer in the load order", id);
					}
				}
			} else if (type == RECORD_STATE) {
				std::uint8_t active = 0;
				a_intfc->ReadRecordData(active);
				Vision::loose = ReadHelmet(a_intfc, version);
				Vision::restorePending = active != 0;
			}
		}
		REX::INFO("SUCCESS: loaded {} helmets", Vision::helmets.size());
	}

	void OnRevert(const F4SE::SerializationInterface*)
	{
		std::scoped_lock guard{ Vision::stateLock };
		Vision::helmets.clear();
		Vision::loose = {};
		Vision::restorePending = false;
		Vision::Shutdown();
	}

	[[nodiscard]] bool Install()
	{
		const auto* serialization = F4SE::GetSerializationInterface();
		if (!serialization) {
			REX::ERROR("F4SE serialization interface is not available");
			return false;
		}
		serialization->SetUniqueID(UNIQUE_ID);
		serialization->SetSaveCallback(OnSave);
		serialization->SetLoadCallback(OnLoad);
		serialization->SetRevertCallback(OnRevert);
		return true;
	}
}

namespace
{
	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		if (a_msg->type == F4SE::MessagingInterface::kGameDataReady) {
			Vision::LoadForms();
			Vision::RegisterOverlay();
		}
	}
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se, {
		.log = true,
		.logName = Version::PROJECT.data(),
		.trampoline = true,
		.trampolineSize = 32,
	});

	Settings::Load();
	Presets::Load();
	Input::Load();
	Sound::Start();

	if (!Save::Install() || !Hooks::Install()) {
		return false;
	}
	Input::Install();

	const auto* messaging = F4SE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnF4SEMessage)) {
		REX::ERROR("failed to register F4SE messaging listener");
		return false;
	}

	REX::INFO("SUCCESS: {} {} loaded", Version::PROJECT, Version::NAME);
	return true;
}

extern "C"
{
	F4SE_EXPORT bool F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
	{
		a_info->name = Version::PROJECT.data();
		a_info->infoVersion = F4SE::PluginInfo::kVersion;
		a_info->version = Version::MAJOR;

		if (a_f4se->IsEditor()) {
			return false;
		}
		return true;
	}
}
