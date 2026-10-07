// vkrun: run a shader IR kernel on a real Vulkan device and compare the result
// with the CPU reference evaluator.
//   usage: vkrun kernel.ir name=1,2,3 other=4,5,6
// Exit codes: 0 = GPU matches CPU, 1 = error or mismatch, 77 = no Vulkan device (skip).
#include "translate.hpp"
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <unistd.h>

#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    std::fprintf(stderr, "error: %s failed (VkResult %d)\n", #call, (int)r_); std::exit(1); } } while (0)

namespace {

constexpr int EXIT_SKIP = 77;

bool parse_inputs(int argc, char** argv, std::map<std::string, std::vector<float>>& inputs, std::string& error) {
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        auto eq = arg.find('=');
        if (eq == std::string::npos) { error = "expected name=values, got '" + arg + "'"; return false; }
        std::vector<float> vals;
        std::istringstream vs(arg.substr(eq + 1));
        for (std::string tok; std::getline(vs, tok, ',');) {
            char* end = nullptr;
            float v = std::strtof(tok.c_str(), &end);
            if (end == tok.c_str() || *end != 0) { error = "bad number '" + tok + "'"; return false; }
            vals.push_back(v);
        }
        inputs[arg.substr(0, eq)] = vals;
    }
    return true;
}

// Assemble SPIR-V text into binary words by running spirv-as.
bool assemble(const std::string& spvasm, std::vector<uint32_t>& words, std::string& error) {
    char asm_path[] = "/tmp/vkrun_XXXXXX";
    int fd = mkstemp(asm_path);
    if (fd < 0) { error = "cannot create temp file"; return false; }
    close(fd);
    std::string spv_path = std::string(asm_path) + ".spv";
    { std::ofstream f(asm_path); f << spvasm; }
    std::string cmd = std::string("spirv-as --target-env vulkan1.0 ") + asm_path + " -o " + spv_path + " 2>&1";
    int rc = std::system(cmd.c_str());
    std::remove(asm_path);
    if (rc != 0) { std::remove(spv_path.c_str()); error = "spirv-as failed (is spirv-tools installed?)"; return false; }
    std::ifstream in(spv_path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    std::remove(spv_path.c_str());
    if (bytes.empty() || bytes.size() % 4 != 0) { error = "spirv-as produced no valid output"; return false; }
    words.resize(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return true;
}

struct Buffer { VkBuffer buf; VkDeviceMemory mem; float* map; };

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: vkrun <file.ir> [name=v1,v2,...]...\n"; return 1; }
    std::ifstream f(argv[1]);
    if (!f) { std::cerr << "error: cannot open " << argv[1] << "\n"; return 1; }
    std::string text((std::istreambuf_iterator<char>(f)), {});

    // ---- CPU side: parse, evaluate (the expected answer), translate, assemble ----
    Program prog;
    std::string err;
    if (!parse_ir(text, prog, err)) { std::cerr << "error: " << err << "\n"; return 1; }
    std::map<std::string, std::vector<float>> inputs;
    if (!parse_inputs(argc, argv, inputs, err)) { std::cerr << "error: " << err << "\n"; return 1; }
    std::vector<float> expected;
    if (!evaluate(prog, inputs, expected, err)) { std::cerr << "error: " << err << "\n"; return 1; }
    const size_t n = expected.size();
    if (n == 0) { std::cerr << "error: no elements to process\n"; return 1; }
    std::vector<uint32_t> spirv;
    if (!assemble(emit_spvasm(prog), spirv, err)) { std::cerr << "error: " << err << "\n"; return 1; }

    // ---- Vulkan: instance and device ------------------------------------------
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vkrun";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("no Vulkan instance available\n");
        return EXIT_SKIP;
    }
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(instance, &ndev, nullptr);
    if (ndev == 0) { std::printf("no Vulkan device found\n"); return EXIT_SKIP; }
    std::vector<VkPhysicalDevice> devs(ndev);
    vkEnumeratePhysicalDevices(instance, &ndev, devs.data());
    size_t pick = std::getenv("VKRUN_DEVICE") ? (size_t)std::atoi(std::getenv("VKRUN_DEVICE")) : 0;
    if (pick >= ndev) pick = 0;
    for (uint32_t i = 0; i < ndev; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        std::printf("%s device %u: %s\n", i == pick ? "using" : "     ", i, props.deviceName);
    }
    VkPhysicalDevice phys = devs[pick];

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qprops.data());
    uint32_t qf = nq;
    for (uint32_t i = 0; i < nq; ++i)
        if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
    if (qf == nq) { std::printf("device has no compute queue\n"); return EXIT_SKIP; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    VK(vkCreateDevice(phys, &dci, nullptr, &dev));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    // ---- Buffers: one per input, one for the output, padded to whole workgroups ----
    // The shader has no bounds check, so every thread of the last group must have
    // memory to touch. Round the element count up to a multiple of the group size (64).
    const uint32_t groups = (uint32_t)((n + 63) / 64);
    const size_t padded = (size_t)groups * 64;

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    auto find_type = [&](uint32_t bits, VkMemoryPropertyFlags want) -> uint32_t {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        std::fprintf(stderr, "error: no host-visible coherent memory type\n");
        std::exit(1);
    };
    auto make_buffer = [&]() {
        Buffer b{};
        const VkDeviceSize bytes = padded * sizeof(float);
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK(vkCreateBuffer(dev, &bci, nullptr, &b.buf));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, b.buf, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = find_type(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK(vkAllocateMemory(dev, &mai, nullptr, &b.mem));
        VK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
        void* p = nullptr;
        VK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &p));
        b.map = static_cast<float*>(p);
        std::memset(b.map, 0, bytes);
        return b;
    };

    std::vector<Buffer> bufs;                       // bindings 0..k-1 = inputs, k = output
    for (const auto& name : prog.inputs) {
        Buffer b = make_buffer();
        const auto& data = inputs.at(name);
        std::memcpy(b.map, data.data(), data.size() * sizeof(float));
        bufs.push_back(b);
    }
    bufs.push_back(make_buffer());
    // Fill the output with a sentinel: threads past the end of the data must leave it untouched.
    const float SENTINEL = 12345.0f;
    for (size_t i = 0; i < padded; ++i) bufs.back().map[i] = SENTINEL;
    const uint32_t nb = (uint32_t)bufs.size();

    // ---- Descriptors and pipeline ----------------------------------------------
    std::vector<VkDescriptorSetLayoutBinding> binds(nb);
    for (uint32_t i = 0; i < nb; ++i) {
        binds[i] = VkDescriptorSetLayoutBinding{};
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = nb;
    dslci.pBindings = binds.data();
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    VkPushConstantRange pcr{};                      // the element count, read by the shader's bounds check
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(uint32_t);
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VK(vkCreatePipelineLayout(dev, &plci, nullptr, &layout));

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spirv.size() * sizeof(uint32_t);
    smci.pCode = spirv.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    VK(vkCreateShaderModule(dev, &smci, nullptr, &shader));

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = stage;
    cpci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));

    VkDescriptorPoolSize psize{};
    psize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    psize.descriptorCount = nb;
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &psize;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VK(vkCreateDescriptorPool(dev, &dpci, nullptr, &pool));

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK(vkAllocateDescriptorSets(dev, &dsai, &set));

    std::vector<VkDescriptorBufferInfo> infos(nb);
    std::vector<VkWriteDescriptorSet> writes(nb);
    for (uint32_t i = 0; i < nb; ++i) {
        infos[i] = VkDescriptorBufferInfo{bufs[i].buf, 0, VK_WHOLE_SIZE};
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(dev, nb, writes.data(), 0, nullptr);

    // ---- Record, submit, wait ----------------------------------------------------
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = qf;
    VkCommandPool cmdpool = VK_NULL_HANDLE;
    VK(vkCreateCommandPool(dev, &cpi, nullptr, &cmdpool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK(vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(cb, &bi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    const uint32_t count = (uint32_t)n;
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count), &count);
    vkCmdDispatch(cb, groups, 1, 1);
    // Make the shader's writes visible to the host before we read them back.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    VK(vkEndCommandBuffer(cb));

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VK(vkCreateFence(dev, &fci, nullptr, &fence));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VK(vkQueueSubmit(queue, 1, &si, fence));
    VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

    // ---- Compare GPU output with the CPU reference ------------------------------
    const float* gpu = bufs.back().map;
    bool ok = true;
    size_t first_bad = 0;
    for (size_t i = 0; i < n; ++i) {
        float g = gpu[i], e = expected[i];
        if (!(std::fabs(g - e) <= 1e-5f + 1e-5f * std::fabs(e))) { ok = false; first_bad = i; break; }
    }
    auto print_vec = [&](const char* label, const float* v) {
        std::printf("%s:", label);
        size_t shown = n < 16 ? n : 16;
        for (size_t i = 0; i < shown; ++i) std::printf(" %g", (double)v[i]);
        if (n > shown) std::printf(" ... (%zu elements)", n);
        std::printf("\n");
    };
    size_t first_dirty = padded;                    // first padding element that was written to
    for (size_t i = n; i < padded; ++i)
        if (gpu[i] != SENTINEL) { first_dirty = i; break; }
    print_vec("gpu", gpu);
    print_vec("cpu", expected.data());
    if (!ok) { std::printf("MISMATCH at element %zu\n", first_bad); return 1; }
    if (first_dirty != padded) {
        std::printf("PADDING OVERWRITTEN at element %zu (threads past the end wrote to the buffer)\n", first_dirty);
        return 1;
    }
    std::printf("match (%zu elements, padding untouched)\n", n);
    return 0;   // process exit releases the Vulkan objects; explicit teardown can come later
}
