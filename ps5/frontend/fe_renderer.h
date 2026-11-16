// PS5 port frontend: the Vulkan renderer. Two passes a frame: the scene (background, reflections,
// the PS2 cases and the selection's outline glow) into an offscreen image, then FXAA from it into
// the display image with the UI (text and shapes) on top.
//
// Written for what the PS5 driver runs: classic render passes, no MSAA (its resolve is a CPU copy),
// no mip chains, no image queries in shaders, 16-bit indices, push constants of 128 bytes.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_math.h"
#include "fe_text.h"
#include "fe_vk.h"

#include <string>
#include <vector>

namespace fe
{
struct Texture
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	uint32_t width = 0, height = 0;
};

struct BoxDraw
{
	Mat4 model;
	VkDescriptorSet set = VK_NULL_HANDLE; // cover, placeholder, spine, back
	float brightness = 1, cover_mix = 0, selected = 0;
	float sheen_pos = 0, sheen_strength = 0, alpha = 1;
};

struct HaloDraw
{
	bool enabled = false;
	Mat4 model;
	float half_w = 0, half_h = 0, z = 0, margin = 0;
	float color[4] = {1, 1, 1, 1}; // rgb, intensity
	float radius = 0.04f, line = 0.012f, glow_width = 0.08f;
};

// ps2-console-shelf (AI-assisted): what shaders/bg.frag takes (its push constants, in this order): the PS2 fog.
struct BgParams
{
	float glow_color[4]; // rgb, strength: the white bloom behind the selection
	float glow_pos[4];   // x, y (0..1), radius x, radius y (of the height)
	float glow2[4];      // x, y (0..1), radius, strength: a bluish bloom (the disc)
	float fog_lo[4];     // the fog's dark colour; w: how much of the Browser's lit grey field
	float fog_hi[4];     // the fog's light colour; w: how much it gathers in the middle
	float motes[4];      // the motes' colour; w: their strength
	float misc[4];       // aspect, top-left corner light, vignette, time
};

// Texture changes within the UI vertex stream (AI-assisted).
struct UiImageRange
{
	uint32_t first = 0, count = 0;
	VkDescriptorSet set = VK_NULL_HANDLE;
};

struct FrameDesc
{
	Mat4 view_proj;
	Vec3 cam_pos;
	float time = 0;
	float glow[4] = {0.5f, 0.4f, 1.0f, 1.0f};
	float floor_y = -1, reflect_strength = 0.3f, reflect_falloff = 2.0f;
	BgParams bg = {};
	std::vector<BoxDraw> reflections;
	std::vector<BoxDraw> boxes;
	HaloDraw halo;
	std::vector<UiVertex> ui;
	std::vector<UiImageRange> ui_images;
	float fade = 1.0f;
	// P2-19e: the first `ui_scene` UI vertices (a screen's 3D field: the sheets' rods and cubes) are drawn into the scene, under the
	// post pass, which can soften them (`blur` 0 sharp .. 1 soft, ~4 px at 1080 lines); the rest (the words) stay sharp over it.
	uint32_t ui_scene = 0;
	float blur = 0.0f;
	uint32_t ui_under = 0; // shelf v3 (AI-assisted): the first ui_under UI vertices are drawn in the scene under the cases (<= ui_scene)
	// shelf v3 (AI-assisted): the PS2 opening's afterimage. ui[0, ghost_end) is the layer fed back (the towers): drawn, then the buffer of the
	// frames before blended over it at 80/128, then fed again (half width). ghost_reset starts the buffer black. 0 = none.
	uint32_t ghost_end = 0;
	bool ghost_reset = false;
};

class Renderer
{
public:
	// shelf v3 (kurolume, 2026-11-16): the cases' plastic, black (the default) or the blue many PAL PS2 games came in.
	void SetCaseBlue(bool blue) { m_case_blue = blue; }
	bool m_case_blue = false;
	// `out_images`: the display images (the swapchain's, or offscreen images on a PC), in
	// `out_format`, `width` x `height`; they end each frame in `out_final_layout`.
	bool Init(Vk* vk, VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family, VkQueue queue,
		uint32_t width, uint32_t height, VkFormat out_format, const std::vector<VkImage>& out_images,
		VkImageLayout out_final_layout);
	void Shutdown();

	// An RGBA8 (4 bytes a pixel) or R8 texture; the upload happens with the next frame.
	Texture* CreateTexture(uint32_t w, uint32_t h, VkFormat format, const void* pixels);
	void DestroyTexture(Texture* t); // freed once the GPU is done with it

	// 2.11: a fourth texture (the case's back; the white one when none is given).
	VkDescriptorSet AllocTextureSet(Texture* t0, Texture* t1, Texture* t2, Texture* t3 = nullptr);
	void FreeTextureSet(VkDescriptorSet set);

	void SetAtlas(Texture* atlas);

	// Waits until the next frame's resources (its command buffer and the semaphores the platform
	// pairs with it) are free; Render does this itself when it wasn't called.
	bool WaitForSlot();

	// Renders into display image `out_index`, waiting for `wait` and signalling `signal` (either may
	// be null). Returns false on a Vulkan error (error() says which).
	bool Render(const FrameDesc& f, uint32_t out_index, VkSemaphore wait, VkSemaphore signal);

	// PC only: the display image as RGBA8 rows (its format must be R8G8B8A8 or B8G8R8A8).
	bool ReadPixels(uint32_t out_index, std::vector<uint8_t>& rgba);

	void WaitIdle();
	const std::string& error() const { return m_error; }

	// shelf v3 (AI-assisted): the scene image (the frame's 3D pass, before the post pass) as a texture set for the final pass's UI: what
	// the PS2's glass refracts.
	VkDescriptorSet SceneSet() const { return m_post_set; }
	uint32_t width() const { return m_width; }
	uint32_t height() const { return m_height; }

private:
	struct Buffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mapped = nullptr;
		VkDeviceSize size = 0;
	};
	struct Upload
	{
		Texture* texture;
		Buffer staging;
	};
	struct Slot
	{
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		Buffer ubo;
		Buffer ui;
		VkDescriptorSet frame_set = VK_NULL_HANDLE;
		std::vector<Buffer> dead_buffers;
		std::vector<Texture*> dead_textures;
		std::vector<VkDescriptorSet> dead_sets;
		bool submitted = false;
	};

	bool Fail(const char* what, VkResult r);
	uint32_t FindMemory(uint32_t type_bits, VkMemoryPropertyFlags want) const;
	bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);
	void DestroyBuffer(Buffer& b);
	bool CreateImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
		Texture& out);
	void DestroyImage(Texture& t);
	bool CreatePipelines();
	VkShaderModule Module(const uint32_t* words, size_t bytes);
	bool CreateBoxMesh();
	void Reclaim(Slot& s);
	Slot& RetireSlot(); // where a resource retired now waits: the last submitted frame's slot
	void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
		VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);

	Vk* m_vk = nullptr;
	VkPhysicalDevice m_pd = VK_NULL_HANDLE;
	VkDevice m_dev = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;
	uint32_t m_queue_family = 0;
	VkPhysicalDeviceMemoryProperties m_mem = {};
	uint32_t m_width = 0, m_height = 0;
	VkFormat m_out_format = VK_FORMAT_UNDEFINED;
	VkImageLayout m_out_final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	std::vector<VkImage> m_out_images;
	std::vector<VkImageView> m_out_views;
	std::vector<VkFramebuffer> m_out_fbs;

	Texture m_scene;
	Texture m_depth;
	VkFramebuffer m_scene_fb = VK_NULL_HANDLE;
	VkRenderPass m_scene_pass = VK_NULL_HANDLE;
	VkRenderPass m_final_pass = VK_NULL_HANDLE;
	// shelf v3 (AI-assisted): the PS2 opening's afterimage (boot.md 2.3): a half-width buffer fed with the towers each frame and blended
	// back at 80/128 under the next frame's.
	VkRenderPass m_ghost_load_pass = VK_NULL_HANDLE;
	Texture m_ghost_g, m_ghost_t, m_ghost_depth;
	VkFramebuffer m_ghost_g_fb = VK_NULL_HANDLE, m_ghost_t_fb = VK_NULL_HANDLE;
	VkDescriptorSet m_ghost_g_set = VK_NULL_HANDLE, m_ghost_t_set = VK_NULL_HANDLE;
	bool m_ghost_valid = false;
	bool EnsureGhost();

	VkDescriptorSetLayout m_set0_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_set1_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_layout = VK_NULL_HANDLE;
	VkDescriptorPool m_pool = VK_NULL_HANDLE;
	VkSampler m_sampler = VK_NULL_HANDLE;

	VkPipeline m_bg = VK_NULL_HANDLE;
	VkPipeline m_box = VK_NULL_HANDLE;
	VkPipeline m_box_reflect = VK_NULL_HANDLE;
	VkPipeline m_halo = VK_NULL_HANDLE;
	VkPipeline m_post = VK_NULL_HANDLE;
	VkPipeline m_ui = VK_NULL_HANDLE;
	VkPipeline m_ui_scene = VK_NULL_HANDLE; // P2-19e: the UI pipeline in the scene pass (the field under the soft focus)

	Buffer m_box_vb, m_box_ib;
	Buffer m_quad_vb; // the full-screen triangle, then the halo's six corners
	uint32_t m_box_index_count = 0;

	Texture* m_white = nullptr;
	Texture* m_atlas = nullptr;
	VkDescriptorSet m_post_set = VK_NULL_HANDLE;
	VkDescriptorSet m_atlas_set = VK_NULL_HANDLE;

	VkCommandPool m_cmd_pool = VK_NULL_HANDLE;
	static constexpr int kSlots = 2;
	Slot m_slots[kSlots];
	int m_slot = 0;
	std::vector<Upload> m_pending;
	std::vector<bool> m_out_initialized;

	std::string m_error;
};

// The case's half extents in world units (1 unit = 100 mm): a PS2 DVD case is 135 x 190 x 14 mm.
constexpr float kBoxHalfW = 0.675f, kBoxHalfH = 0.95f, kBoxHalfD = 0.07f;
} // namespace fe
