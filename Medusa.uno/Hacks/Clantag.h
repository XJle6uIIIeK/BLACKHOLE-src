#pragma once

#include "../JsonForward.h"

#include "../imgui/imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS

#include "../imgui/imgui_internal.h"
#include "../imgui/imgui_stdlib.h"
#include "../ConfigStructs.h"

namespace Clan {

	struct neededVars {
		int index;
		std::string name;
	};

	std::vector<neededVars>players;


	void update(bool reset = false, bool update = false) noexcept;
	void SetStealFromIdx(int idx);
	void SetStealEnabled(bool enabled);
};

namespace ClanTagStealer {
	void update() noexcept;
};