// laptop-preview (AI-assisted): fe_host's live window. The shelf still renders into fe_host's two offscreen images; each frame
// the one just drawn is scaled (letterboxed at 16:9) into an SDL3 window's Vulkan swapchain and presented (FIFO: the display's
// refresh rate). Keyboard and DualShock 4 / DualSense (SDL's gamepad API, USB or Bluetooth) become fe::Input as fe_ps5.cpp reads
// the PS5's pad. The shelf's sounds (fe::Mixer) play through SDL's audio. Only built with FE_HOST_WINDOW (build-host.sh, FE_HOST_WINDOW=1).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../fe_app.h"
#include "../fe_sound.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

struct HostWindow
{
	SDL_Window* win = nullptr;
	SDL_AudioStream* audio = nullptr;
	std::vector<SDL_Gamepad*> pads;
	bool fullscreen = false;
	bool resized = true;
	bool quit = false;

	// Vulkan
	fe::Vk* vk = nullptr;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t qf = 0;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
	VkExtent2D extent = {0, 0};
	std::vector<VkImage> images;
	std::vector<VkSemaphore> done_sems; // one per swapchain image
	VkCommandPool pool = VK_NULL_HANDLE;
	static constexpr int kSlots = 2;
	VkCommandBuffer cmds[kSlots] = {};
	VkFence fences[kSlots] = {};
	VkSemaphore acquire_sems[kSlots] = {};
	int slot = 0;

	PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetSurfaceSupport = nullptr;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetSurfaceCaps = nullptr;
	PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetSurfaceFormats = nullptr;
	PFN_vkDestroySurfaceKHR DestroySurface = nullptr;
	PFN_vkCmdBlitImage CmdBlitImage = nullptr;
	PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;

	bool Open(int w, int h, bool full)
	{
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
		SDL_SetHint(SDL_HINT_APP_ID, "ps2-shelf-preview");
		SDL_SetHint(SDL_HINT_APP_NAME, "PS2 Shelf Preview"); // the sound's name in the mixer // the Wayland app id (the .desktop file's StartupWMClass)
		if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
		{
			std::fprintf(stderr, "[window] SDL_Init: %s\n", SDL_GetError());
			return false;
		}
		win = SDL_CreateWindow("PS2 Shelf Preview", w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
		if (!win)
		{
			std::fprintf(stderr, "[window] SDL_CreateWindow: %s\n", SDL_GetError());
			return false;
		}
		SDL_SetWindowAspectRatio(win, 16.0f / 9.0f, 16.0f / 9.0f);
		SDL_SetWindowMinimumSize(win, 480, 270);
		if (full)
			ToggleFullscreen();
		std::printf("[window] video driver: %s\n", SDL_GetCurrentVideoDriver());
		return true;
	}

	void SetTitle(const std::string& t) { SDL_SetWindowTitle(win, t.c_str()); }

	void ToggleFullscreen()
	{
		fullscreen = !fullscreen;
		SDL_SetWindowFullscreen(win, fullscreen);
		resized = true;
	}

	std::vector<const char*> InstanceExtensions() const
	{
		Uint32 n = 0;
		const char* const* e = SDL_Vulkan_GetInstanceExtensions(&n);
		return std::vector<const char*>(e, e + n);
	}

	// After the instance: the surface and the entry points fe::Vk leaves out.
	bool CreateSurface(VkInstance inst, PFN_vkGetInstanceProcAddr gipa)
	{
		instance = inst;
		GetSurfaceSupport = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(gipa(inst, "vkGetPhysicalDeviceSurfaceSupportKHR"));
		GetSurfaceCaps = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(gipa(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
		GetSurfaceFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(gipa(inst, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
		DestroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(gipa(inst, "vkDestroySurfaceKHR"));
		if (!SDL_Vulkan_CreateSurface(win, inst, nullptr, &surface))
		{
			std::fprintf(stderr, "[window] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
			return false;
		}
		return GetSurfaceSupport && GetSurfaceCaps && GetSurfaceFormats;
	}

	bool CanPresent(VkPhysicalDevice p, uint32_t family) const
	{
		VkBool32 ok = VK_FALSE;
		return GetSurfaceSupport(p, family, surface, &ok) == VK_SUCCESS && ok;
	}

	bool Start(fe::Vk* v, VkPhysicalDevice p, VkDevice d, uint32_t family, VkQueue q)
	{
		vk = v;
		pd = p;
		device = d;
		qf = family;
		queue = q;
		CmdBlitImage = reinterpret_cast<PFN_vkCmdBlitImage>(vk->vkGetDeviceProcAddr(device, "vkCmdBlitImage"));
		CmdClearColorImage = reinterpret_cast<PFN_vkCmdClearColorImage>(vk->vkGetDeviceProcAddr(device, "vkCmdClearColorImage"));
		VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = qf;
		if (vk->vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS)
			return false;
		VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = kSlots;
		if (vk->vkAllocateCommandBuffers(device, &ai, cmds) != VK_SUCCESS)
			return false;
		for (int i = 0; i < kSlots; i++)
		{
			VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			if (vk->vkCreateFence(device, &fci, nullptr, &fences[i]) != VK_SUCCESS ||
				vk->vkCreateSemaphore(device, &sci, nullptr, &acquire_sems[i]) != VK_SUCCESS)
				return false;
		}
		return CmdBlitImage && CmdClearColorImage && Recreate();
	}

	bool Recreate()
	{
		vk->vkDeviceWaitIdle(device);
		VkSurfaceCapabilitiesKHR caps;
		if (GetSurfaceCaps(pd, surface, &caps) != VK_SUCCESS)
			return false;
		int pw = 0, ph = 0;
		SDL_GetWindowSizeInPixels(win, &pw, &ph);
		extent = caps.currentExtent;
		if (extent.width == 0xFFFFFFFFu)
			extent = {static_cast<uint32_t>(pw), static_cast<uint32_t>(ph)};
		extent.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, extent.width));
		extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, extent.height));
		if (extent.width == 0 || extent.height == 0)
			return true; // minimised: try again later
		uint32_t nf = 0;
		GetSurfaceFormats(pd, surface, &nf, nullptr);
		std::vector<VkSurfaceFormatKHR> fmts(nf);
		GetSurfaceFormats(pd, surface, &nf, fmts.data());
		VkSurfaceFormatKHR pick = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
		for (const VkSurfaceFormatKHR& f : fmts) // the shelf's pixels are already display values: an UNORM target keeps them as they are
			if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				pick = f;
				break;
			}
		format = pick.format;
		VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
		ci.surface = surface;
		ci.minImageCount = std::max(caps.minImageCount, 3u);
		if (caps.maxImageCount)
			ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
		ci.imageFormat = pick.format;
		ci.imageColorSpace = pick.colorSpace;
		ci.imageExtent = extent;
		ci.imageArrayLayers = 1;
		ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ci.preTransform = caps.currentTransform;
		ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR :
		                    static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
		ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync: the display's refresh rate
		ci.clipped = VK_TRUE;
		ci.oldSwapchain = swapchain;
		VkSwapchainKHR next = VK_NULL_HANDLE;
		if (vk->vkCreateSwapchainKHR(device, &ci, nullptr, &next) != VK_SUCCESS)
		{
			std::fprintf(stderr, "[window] vkCreateSwapchainKHR failed\n");
			return false;
		}
		if (swapchain)
			vk->vkDestroySwapchainKHR(device, swapchain, nullptr);
		swapchain = next;
		uint32_t n = 0;
		vk->vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
		images.resize(n);
		vk->vkGetSwapchainImagesKHR(device, swapchain, &n, images.data());
		for (VkSemaphore s : done_sems)
			vk->vkDestroySemaphore(device, s, nullptr);
		done_sems.assign(n, VK_NULL_HANDLE);
		for (VkSemaphore& s : done_sems)
		{
			VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			vk->vkCreateSemaphore(device, &sci, nullptr, &s);
		}
		resized = false;
		return true;
	}

	// The offscreen image `src` (w x h, TRANSFER_SRC_OPTIMAL, just rendered on the same queue) into the window, letterboxed.
	bool Present(VkImage src, uint32_t w, uint32_t h)
	{
		if (resized && !Recreate())
			return false;
		if (extent.width == 0 || extent.height == 0)
		{
			SDL_Delay(16);
			resized = true;
			return true;
		}
		vk->vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX);
		uint32_t idx = 0;
		VkResult r = vk->vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquire_sems[slot], VK_NULL_HANDLE, &idx);
		if (r == VK_ERROR_OUT_OF_DATE_KHR)
		{
			resized = true;
			return true;
		}
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
			return false;
		vk->vkResetFences(device, 1, &fences[slot]);
		VkCommandBuffer cmd = cmds[slot];
		vk->vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vk->vkBeginCommandBuffer(cmd, &bi);
		const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageMemoryBarrier b[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
		b[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // the renderer's frame, earlier on this queue
		b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		b[0].oldLayout = b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b[0].image = src;
		b[0].subresourceRange = range;
		b[1].srcAccessMask = 0;
		b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b[1].srcQueueFamilyIndex = b[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b[1].image = images[idx];
		b[1].subresourceRange = range;
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
		VkClearColorValue black = {};
		CmdClearColorImage(cmd, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		VkImageMemoryBarrier c = b[1];
		c.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		c.oldLayout = c.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &c);
		// 16:9 inside the window, centred
		const double sx = static_cast<double>(extent.width) / w, sy = static_cast<double>(extent.height) / h, s = std::min(sx, sy);
		const int32_t dw = static_cast<int32_t>(std::lround(w * s)), dh = static_cast<int32_t>(std::lround(h * s));
		const int32_t dx = (static_cast<int32_t>(extent.width) - dw) / 2, dy = (static_cast<int32_t>(extent.height) - dh) / 2;
		VkImageBlit blit = {};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.srcOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
		blit.dstSubresource = blit.srcSubresource;
		blit.dstOffsets[0] = {dx, dy, 0};
		blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
		CmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
		VkImageMemoryBarrier e[2] = {b[0], b[1]};
		e[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; // the renderer draws into it again two frames on
		e[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
		e[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		e[1].dstAccessMask = 0;
		e[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		e[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, e);
		vk->vkEndCommandBuffer(cmd);
		const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &acquire_sems[slot];
		si.pWaitDstStageMask = &wait_stage;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &done_sems[idx];
		if (vk->vkQueueSubmit(queue, 1, &si, fences[slot]) != VK_SUCCESS)
			return false;
		VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &done_sems[idx];
		pi.swapchainCount = 1;
		pi.pSwapchains = &swapchain;
		pi.pImageIndices = &idx;
		r = vk->vkQueuePresentKHR(queue, &pi);
		if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
			resized = true;
		else if (r != VK_SUCCESS)
			return false;
		slot = (slot + 1) % kSlots;
		return true;
	}

	// ---- sound ----
	static void SDLCALL AudioCallback(void* user, SDL_AudioStream* stream, int additional, int)
	{
		static thread_local std::vector<float> buf;
		const int frames = additional / static_cast<int>(sizeof(float) * 2);
		if (frames <= 0)
			return;
		buf.resize(static_cast<size_t>(frames) * 2);
		static_cast<fe::Mixer*>(user)->Mix(buf.data(), frames);
		SDL_PutAudioStreamData(stream, buf.data(), frames * static_cast<int>(sizeof(float) * 2));
	}
	bool StartAudio(fe::Mixer* mixer)
	{
		SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, mixer->OutputRate()}; // (FE_HOST_AUDIO_RATE: the stream at another rate, the mixer converting)
		audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, AudioCallback, mixer);
		if (!audio)
		{
			std::fprintf(stderr, "[window] no audio: %s\n", SDL_GetError());
			return false;
		}
		SDL_ResumeAudioStreamDevice(audio);
		std::printf("[window] audio: %s\n", SDL_GetCurrentAudioDriver());
		return true;
	}

	// ---- input ----
	void PadAdded(SDL_JoystickID id)
	{
		if (SDL_Gamepad* g = SDL_OpenGamepad(id))
		{
			pads.push_back(g);
			std::printf("[window] controller: %s\n", SDL_GetGamepadName(g));
		}
	}
	void PadRemoved(SDL_JoystickID id)
	{
		for (size_t i = 0; i < pads.size(); i++)
			if (SDL_GetGamepadID(pads[i]) == id)
			{
				SDL_CloseGamepad(pads[i]);
				pads.erase(pads.begin() + static_cast<std::ptrdiff_t>(i));
				return;
			}
	}
	bool PadTouch() const
	{
		for (SDL_Gamepad* g : pads)
			if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_TOUCHPAD))
				return true;
		return false;
	}
	std::string PadNames() const
	{
		std::string s;
		for (SDL_Gamepad* g : pads)
			s += (s.empty() ? "" : ", ") + std::string(SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "controller");
		return s;
	}

	// The keyboard and every controller, as fe_ps5.cpp reads the PS5's pad (sticks past 48/208 of 0..255 count as the D-pad).
	fe::Input ReadInput() const
	{
		fe::Input in;
		const bool* k = SDL_GetKeyboardState(nullptr);
		in.left = k[SDL_SCANCODE_LEFT];
		in.right = k[SDL_SCANCODE_RIGHT];
		in.up = k[SDL_SCANCODE_UP];
		in.down = k[SDL_SCANCODE_DOWN];
		in.cross = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER] || k[SDL_SCANCODE_X];
		in.circle = k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_ESCAPE];
		in.square = k[SDL_SCANCODE_S];
		in.triangle = k[SDL_SCANCODE_T];
		in.l1 = k[SDL_SCANCODE_Q];
		in.r1 = k[SDL_SCANCODE_E];
		in.l2 = k[SDL_SCANCODE_1];
		in.r2 = k[SDL_SCANCODE_3];
		in.options = k[SDL_SCANCODE_TAB];
		in.r3 = k[SDL_SCANCODE_R] || k[SDL_SCANCODE_C];
		in.rx = (k[SDL_SCANCODE_L] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_J] ? 1.0f : 0.0f);
		in.ry = (k[SDL_SCANCODE_K] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_I] ? 1.0f : 0.0f);
		const float edge = (128.0f - 48.0f) / 127.0f;      // 48 of 0..255
		const float trig = 160.0f / 255.0f;                // 160 of 0..255
		for (SDL_Gamepad* g : pads)
		{
			auto btn = [g](SDL_GamepadButton b) { return SDL_GetGamepadButton(g, b); };
			auto axis = [g](SDL_GamepadAxis a) { return std::max(-1.0f, SDL_GetGamepadAxis(g, a) / 32767.0f); };
			const float lx = axis(SDL_GAMEPAD_AXIS_LEFTX), ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
			in.left |= btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -edge;
			in.right |= btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > edge;
			in.up |= btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -edge;
			in.down |= btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > edge;
			in.cross |= btn(SDL_GAMEPAD_BUTTON_SOUTH);
			in.circle |= btn(SDL_GAMEPAD_BUTTON_EAST);
			in.square |= btn(SDL_GAMEPAD_BUTTON_WEST);
			in.triangle |= btn(SDL_GAMEPAD_BUTTON_NORTH);
			in.l1 |= btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
			in.r1 |= btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
			in.l2 |= axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > trig;
			in.r2 |= axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > trig;
			in.options |= btn(SDL_GAMEPAD_BUTTON_START);
			in.r3 |= btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
			const float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
			if (std::fabs(rx) > std::fabs(in.rx))
				in.rx = rx;
			if (std::fabs(ry) > std::fabs(in.ry))
				in.ry = ry;
		}
		return in;
	}

	void Close()
	{
		if (audio)
			SDL_DestroyAudioStream(audio);
		audio = nullptr;
		if (device)
		{
			vk->vkDeviceWaitIdle(device);
			for (int i = 0; i < kSlots; i++)
			{
				vk->vkDestroyFence(device, fences[i], nullptr);
				vk->vkDestroySemaphore(device, acquire_sems[i], nullptr);
			}
			for (VkSemaphore s : done_sems)
				vk->vkDestroySemaphore(device, s, nullptr);
			if (pool)
				vk->vkDestroyCommandPool(device, pool, nullptr);
			if (swapchain)
				vk->vkDestroySwapchainKHR(device, swapchain, nullptr);
		}
		if (surface && DestroySurface)
			DestroySurface(instance, surface, nullptr);
		for (SDL_Gamepad* g : pads)
			SDL_CloseGamepad(g);
		if (win)
			SDL_DestroyWindow(win);
		SDL_Quit();
	}
};
