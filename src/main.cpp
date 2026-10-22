#include <iostream>
#include <spdlog/spdlog.h>

#include "nova.h"

#include <tiny_obj_loader.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/glm.hpp>

struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
};

struct ShaderData {
	glm::mat4 projection;
	glm::mat4 view;
	glm::mat4 model[3];
	glm::vec4 lightPos = { 0.0f, -10.0f, 10.0f, 0.0f };
	uint32_t selected = 1;
};

static constexpr uint32_t maxFramesInFlight = 2;

bool updateSwapchain = false;
uint32_t selectedObject = 1;

int main() {
    nova::Instance instance;
    nova::Window window(instance, 800, 600, "Nova Engine");
    nova::Device device(instance);

    nova::FramebufferResizeCallback framebufferResizeCallback = [](nova::Window* window, int width, int height) {
        updateSwapchain = true;
    };

    nova::KeyInputCallback keyCallback = [](nova::Window* window, nova::KeyCode key, nova::KeyModifierFlag mods, nova::KeyAction action) {
        if (action == nova::KeyAction::Press) {
            if (key == nova::KeyCode::Plus || key == nova::KeyCode::Equals) {
                selectedObject = (selectedObject + 1) % 3;
            } else if (key == nova::KeyCode::Minus || key == nova::KeyCode::Underscore) {
                selectedObject = (selectedObject + 2) % 3;
            }
        }
    };

    window.SetKeyInputCallback(keyCallback);
    window.SetFramebufferResizeCallback(framebufferResizeCallback);

    nova::Swapchain swapchain = device.CreateSwapchainBuilder(window.GetSurface())
        .SetDesiredFormat(nova::ImageFormat::B8G8R8A8Unorm)
        .SetDesiredColorSpace(nova::ColorSpace::SrgbNonlinear)
        .SetDesiredPresentMode(nova::SwapchainPresentMode::Fifo)
        .SetDesiredExtent(window.GetFramebufferSize())
        .build();

    nova::Image depthImage = device.CreateImage({
        .format = device.GetSupportedDepthFormats()[0],
        .extent = { window.GetFramebufferWidth(), window.GetFramebufferHeight(), 1 },
        .usage = nova::ImageUsageFlag::DepthStencilAttachment
    });

    nova::ImageView depthImageView = depthImage.CreateView(nova::ImageAspectFlag::Depth);

    // load model

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    tinyobj::LoadObj(&attrib, &shapes, &materials, nullptr, nullptr, "res/suzanne.obj");
    const VkDeviceSize indexCount{shapes[0].mesh.indices.size()};   

    std::vector<Vertex> vertices{};
    std::vector<uint16_t> indices{};

    for (auto& index : shapes[0].mesh.indices) {
        Vertex v{
            .pos = { attrib.vertices[index.vertex_index * 3], -attrib.vertices[index.vertex_index * 3 + 1], attrib.vertices[index.vertex_index * 3 + 2] },
            .normal = { attrib.normals[index.normal_index * 3], -attrib.normals[index.normal_index * 3 + 1], attrib.normals[index.normal_index * 3 + 2] },
            .uv = { attrib.texcoords[index.texcoord_index * 2], 1.0 - attrib.texcoords[index.texcoord_index * 2 + 1] }
        };
        vertices.push_back(v);
        indices.push_back(indices.size());
    }

    // allocate buffers and copy data

    nova::AllocationFlag allocationFlags = 
        nova::AllocationFlag::HostAccessSequentialWrite | 
        // nova::AllocationFlag::HostAccessAllowTransfer | 
        nova::AllocationFlag::Mapped;
    size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
    size_t indexBufferSize = indices.size() * sizeof(uint16_t);
    size_t bufferSize = vertexBufferSize + indexBufferSize;
    nova::Buffer buffer = device.CreateBuffer(bufferSize, nova::BufferUsageFlag::VertexBuffer | nova::BufferUsageFlag::IndexBuffer, allocationFlags);
    memcpy(buffer.GetMappedData(), vertices.data(), vertices.size() * sizeof(Vertex));
    memcpy(static_cast<uint8_t*>(buffer.GetMappedData()) + vertexBufferSize, indices.data(), indices.size() * sizeof(uint16_t));


    nova::CommandPool commandPool = device.CreateCommandPool(nova::CommandPoolCreateFlag::ResetCommandBuffer);

    std::array<nova::Buffer, maxFramesInFlight> shaderDataBuffers;
    std::array<nova::CommandBuffer, maxFramesInFlight> commandBuffers;
    std::array<nova::Fence, maxFramesInFlight> fences;
    std::array<nova::Semaphore, maxFramesInFlight> renderSemaphores;
    std::vector<nova::Semaphore> presentSemaphores;

    for (size_t i = 0; i < maxFramesInFlight; ++i) {
        shaderDataBuffers[i] = device.CreateBuffer(sizeof(ShaderData), nova::BufferUsageFlag::ShaderDeviceAddress, allocationFlags);
        commandBuffers[i] = device.CreateCommandBuffer(commandPool);

        renderSemaphores[i] = device.CreateSemaphore();
        fences[i] = device.CreateFence(true);
    }

    presentSemaphores.resize(swapchain.GetImages().size());
    for (size_t i = 0; i < presentSemaphores.size(); ++i) {
        presentSemaphores[i] = device.CreateSemaphore();
    }

    // load textures and create bindless descriptor set

    std::array<nova::Texture, 3> textures;
    std::array<nova::Sampler, 3> samplers;
    std::array<nova::ImageView, 3> textureViews;
    for (size_t i = 0; i < textures.size(); ++i) {
        std::string texturePath = "res/suzanne" + std::to_string(i) + ".ktx";
        textures[i] = device.CreateKTXTextureFromFile(texturePath, commandPool);
        samplers[i] = textures[i].CreateLinearSampler();
        textureViews[i] = textures[i].CreateView();
    }

    nova::BindlessTextureDescriptors bindlessTextureDescriptors = device.CreateBindlessTextureDescriptors({
        { &textureViews[0], &samplers[0] },
        { &textureViews[1], &samplers[1] },
        { &textureViews[2], &samplers[2] }
    });

    // compile shader and create graphics pipeline

    nova::ShaderCompiler shaderCompiler;
    shaderCompiler.BeginSession();
    nova::SPIRVData spirv = shaderCompiler.CompileToSPIRVFromShaderPath("res/test1.slang");
    nova::ShaderModule shaderModule = shaderCompiler.CompileToShaderModuleFromShaderPath("res/test1.slang", &device);
    shaderCompiler.EndSession();

    // nova::ShaderCompiler::WriteSPIRVToDisk(spirv, "res/test1.spirv");
    // nova::SPIRVData spirvFromDisk = nova::ShaderCompiler::ReadSPIRVFromDisk("res/test1.spirv");

    nova::GraphicsPipelineBuilder pipelineBuilder = device.CreateGraphicsPipelineBuilder();
    pipelineBuilder
        .AddDescriptorSetLayout(bindlessTextureDescriptors.GetDescriptorSetLayout())
        .AddPushConstantRange({
            .stageFlags = nova::ShaderStageFlag::Vertex,
            .offset = 0,
            .size = sizeof(double)
        })
        .AddVertexBindingLayout({
            .binding = 0,
            .stride = sizeof(float) * 8,
            .attributes = {
                { .format = nova::VertexFormat::Float3, .offset = 0 },
                { .format = nova::VertexFormat::Float3, .offset = sizeof(float) * 3 },
                { .format = nova::VertexFormat::Float2, .offset = sizeof(float) * 6 }
            }
        })
        .AddShaderStage(nova::ShaderStageFlag::Vertex, shaderModule, "main")
        .AddShaderStage(nova::ShaderStageFlag::Fragment, shaderModule, "main")
        .EnableDepthStencil(true, device.GetSupportedDepthFormats()[0])
        .SetColorFormat(nova::ImageFormat::B8G8R8A8Unorm);

    nova::GraphicsPipeline pipeline = pipelineBuilder.Build();

    bool quit = false;
    uint64_t lastTime = window.GetTimeMilliseconds();
    uint32_t frameIndex = 0;
    uint32_t imageIndex = 0;
    ShaderData shaderData{};
    glm::vec3 camPos = { 0.0f, 0.0f, -6.0f };
    glm::vec3 objectRotations[3]{};

    while (!quit) {
        device.WaitForFence(fences[frameIndex]);
        device.ResetFence(fences[frameIndex]);

        const nova::Image swapchainImage = swapchain.AcquireNextImage(imageIndex, updateSwapchain, &renderSemaphores[frameIndex]);

        nova::Extent2D windowSize = window.GetFramebufferSize();
        shaderData.projection = glm::perspective(glm::radians(45.0f), (float)windowSize.width / (float)windowSize.height, 0.1f, 32.0f);
		shaderData.view = glm::translate(glm::mat4(1.0f), camPos);
		for (auto i = 0; i < 3; i++) {
			auto instancePos = glm::vec3((float)(i - 1) * 3.0f, 0.0f, 0.0f);
			shaderData.model[i] = glm::translate(glm::mat4(1.0f), instancePos) * glm::mat4_cast(glm::quat(objectRotations[i]));
		}
        shaderData.selected = selectedObject;
		memcpy(shaderDataBuffers[frameIndex].GetMappedData(), &shaderData, sizeof(ShaderData));

        nova::CommandBuffer& commandBuffer = commandBuffers[frameIndex];
        commandBuffer.Reset();
        commandBuffer.Begin();

        commandBuffer.TransitionImages({
            nova::ImageTransition::ToColorAttachment(swapchainImage, nova::ImageLayout::Undefined),
            nova::ImageTransition::ToDepthStencilAttachment(depthImage, nova::ImageLayout::Undefined)
        });

        commandBuffer.BeginRendering({
            nova::RenderAttachmentInfo::ColorAttachment(swapchain.GetImageViews()[imageIndex], {0.0f, 0.0f, 0.0f, 1.0f}),
            nova::RenderAttachmentInfo::DepthAttachment(depthImageView, {1.0f, 0.0f, 0.0f, 0.0f})
        }, windowSize);

        commandBuffer.SetViewport({0.0f, 0.0f, (float)windowSize.width, (float)windowSize.height, 0.0f, 1.0f});
        commandBuffer.BindGraphicsPipeline(pipeline);
        commandBuffer.SetScissor({0, 0, windowSize.width, windowSize.height});
        commandBuffer.BindDescriptorSet(pipeline.GetPipelineLayout(), bindlessTextureDescriptors.GetDescriptorSet());
        commandBuffer.BindVertexBuffer(buffer);
        commandBuffer.BindIndexBuffer(buffer, vertexBufferSize, nova::IndexType::Uint16);
        auto shaderDataBufferDeviceAddress = shaderDataBuffers[frameIndex].GetBufferDeviceAddress();
        commandBuffer.PushConstants(pipeline.GetPipelineLayout(), nova::ShaderStageFlag::Vertex, 0, sizeof(double), &shaderDataBufferDeviceAddress);
        commandBuffer.DrawIndexed(indexCount, 3);
        commandBuffer.EndRendering();
        commandBuffer.TransitionImage(nova::ImageTransition::ToPresent(swapchainImage, nova::ImageLayout::AttachmentOptimal));
        commandBuffer.End();

        device.SubmitCommandBuffers({
            .commandBuffers = { &commandBuffer },
            .waitSemaphores = { &renderSemaphores[frameIndex] },
            .signalSemaphores = { &presentSemaphores[imageIndex] },
            .waitStageMask = nova::PipelineStageFlag::ColorAttachmentOutput,
            .fence = &fences[frameIndex]
        });

        frameIndex = (frameIndex + 1) % maxFramesInFlight;

        device.Present(swapchain, imageIndex, updateSwapchain, { &presentSemaphores[imageIndex] });

        window.PollEvents();
        float elapsedTime = (window.GetTimeMilliseconds() - lastTime) / 1000.0f;
        lastTime = window.GetTimeMilliseconds();

        if (window.ShouldClose()) {
            quit = true;
        }

        if (window.GetKeyDown(nova::KeyCode::Escape)) {
            quit = true;
        }

        if (window.GetMouseButtonDown(nova::MouseButton::Left)) {
            double dx, dy;
            window.GetMouseMoved(dx, dy);
            objectRotations[shaderData.selected].x -= (float)dy * elapsedTime;
            objectRotations[shaderData.selected].y += (float)dx * elapsedTime;
        }

        double wheelDx, wheelDy;
        window.GetMouseWheelScrolled(wheelDx, wheelDy);
        camPos.z += (float)wheelDy * elapsedTime * 10.0f;

        if (updateSwapchain) {
            updateSwapchain = false;
            device.WaitIdle();
            swapchain.Destroy();
            swapchain = device.CreateSwapchainBuilder(window.GetSurface())
                .SetDesiredFormat(nova::ImageFormat::B8G8R8A8Unorm)
                .SetDesiredColorSpace(nova::ColorSpace::SrgbNonlinear)
                .SetDesiredPresentMode(nova::SwapchainPresentMode::Fifo)
                .SetDesiredExtent(window.GetFramebufferSize())
                .build();
            depthImage.Destroy();
            depthImage = device.CreateImage({
                .format = device.GetSupportedDepthFormats()[0],
                .extent = { window.GetFramebufferWidth(), window.GetFramebufferHeight(), 1 },
                .usage = nova::ImageUsageFlag::DepthStencilAttachment
            });
            depthImageView.Destroy();
            depthImageView = depthImage.CreateView(nova::ImageAspectFlag::Depth);
        }
    }
    device.WaitIdle();
    
    return 0;
}
