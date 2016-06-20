/*
* Vulkan Example -  Rendering a scene with multiple meshes and materials
*
* Copyright (C) 2016 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <vulkan/vulkan.h>
#include "vulkanexamplebase.h"

#define VERTEX_BUFFER_BIND_ID 0
#define ENABLE_VALIDATION false

// Vertex layout used in this example
struct Vertex {
	glm::vec3 pos;
	glm::vec3 normal;
	glm::vec2 uv;
	glm::vec3 color;
};

// Scene related structs

// Shader properites for a material
// Will be passed to the shaders using push constant
struct SceneMaterialProperites
{
	glm::vec4 ambient;
	glm::vec4 diffuse;
	glm::vec4 specular;
	float opacity;
};

// Stores info on the materials used in the scene
struct SceneMaterial
{
	std::string name;
	// Material properties
	SceneMaterialProperites properties;
	// The example only uses a diffuse channel
	vkTools::VulkanTexture diffuse;
	// The material's descriptor contains the material descriptors
	VkDescriptorSet descriptorSet;
	// Pointer to the pipeline used by this material
	VkPipeline *pipeline;
};

// Stores per-mesh Vulkan resources
struct SceneMesh
{
	VkBuffer vertexBuffer;
	VkDeviceMemory vertexMemory;

	VkBuffer indexBuffer;
	VkDeviceMemory indexMemory;
	uint32_t indexCount;

	//VkDescriptorSet descriptorSet;

	// Pointer to the material used by this mesh
	SceneMaterial *material;
};

// Class for loading the scene and generating all Vulkan resources
class Scene
{
private:
	VkDevice device;
	VkQueue queue;

	VkDescriptorPool descriptorPool;

	// We will be using separate descriptor sets (and bindings)
	// for material and scene related uniforms
	struct
	{
		VkDescriptorSetLayout material;
		VkDescriptorSetLayout scene;
	} descriptorSetLayouts;

	VkDescriptorSet descriptorSetScene;

	vkTools::VulkanTextureLoader *textureLoader;

	const aiScene* aScene;

	VkPhysicalDeviceMemoryProperties deviceMemProps;
	uint32_t getMemoryTypeIndex(uint32_t typeBits, VkFlags properties)
	{
		for (int i = 0; i < 32; i++)
		{
			if ((typeBits & 1) == 1)
			{
				if ((deviceMemProps.memoryTypes[i].propertyFlags & properties) == properties)
				{
					return i;
				}
			}
			typeBits >>= 1;
		}
		return 0;
	}

	// Get materials from the assimp scene and map to our scene structures
	void loadMaterials()
	{
		materials.resize(aScene->mNumMaterials);

		for (size_t i = 0; i < materials.size(); i++)
		{
			materials[i] = {};

			aiString name;
			aScene->mMaterials[i]->Get(AI_MATKEY_NAME, name);

			// Properties
			aiColor4D color;
			aScene->mMaterials[i]->Get(AI_MATKEY_COLOR_AMBIENT, color);
			materials[i].properties.ambient = glm::make_vec4(&color.r) + glm::vec4(0.1f);
			aScene->mMaterials[i]->Get(AI_MATKEY_COLOR_DIFFUSE, color);
			materials[i].properties.diffuse = glm::make_vec4(&color.r);
			aScene->mMaterials[i]->Get(AI_MATKEY_COLOR_SPECULAR, color);
			materials[i].properties.specular = glm::make_vec4(&color.r);
			aScene->mMaterials[i]->Get(AI_MATKEY_OPACITY, materials[i].properties.opacity);

			if ((materials[i].properties.opacity) > 0.0f)
				materials[i].properties.specular = glm::vec4(0.0f);

			materials[i].name = name.C_Str();
			std::cout << "Material \"" << materials[i].name << "\"" << std::endl;

			// Textures
			aiString texturefile;
			// Diffuse
			aScene->mMaterials[i]->GetTexture(aiTextureType_DIFFUSE, 0, &texturefile);
			if (aScene->mMaterials[i]->GetTextureCount(aiTextureType_DIFFUSE) > 0)
			{
				std::cout << "  Diffuse: \"" << texturefile.C_Str() << "\"" << std::endl;
				std::string fileName = std::string(texturefile.C_Str());
				std::replace(fileName.begin(), fileName.end(), '\\', '/');
				textureLoader->loadTexture(assetPath + fileName, VK_FORMAT_BC3_UNORM_BLOCK, &materials[i].diffuse);
			}
			else
			{
				std::cout << "  Material has no diffuse, using dummy texture!" << std::endl;
				// todo : separate pipeline and layout
				textureLoader->loadTexture(assetPath + "dummy.ktx", VK_FORMAT_BC2_UNORM_BLOCK, &materials[i].diffuse);
			}

			// For scenes with multiple textures per material we would need to check for additional texture types, e.g.:
			// aiTextureType_HEIGHT, aiTextureType_OPACITY, aiTextureType_SPECULAR, etc.

			// Assign pipeline
			materials[i].pipeline = (materials[i].properties.opacity == 0.0f) ? &pipelines.solid : &pipelines.blending;
		}

		// Generate descriptor sets for the materials

		// Descriptor pool
		std::vector<VkDescriptorPoolSize> poolSizes;
		poolSizes.push_back(vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<uint32_t>(materials.size())));
		poolSizes.push_back(vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(materials.size())));

		VkDescriptorPoolCreateInfo descriptorPoolInfo =
			vkTools::initializers::descriptorPoolCreateInfo(
				static_cast<uint32_t>(poolSizes.size()),
				poolSizes.data(),
				static_cast<uint32_t>(materials.size()) + 1);

		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));

		// Descriptor set and pipeline layouts
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;
		VkDescriptorSetLayoutCreateInfo descriptorLayout;

		// Set 0: Scene matrices
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_SHADER_STAGE_VERTEX_BIT,
			0));
		descriptorLayout = vkTools::initializers::descriptorSetLayoutCreateInfo(
				setLayoutBindings.data(),
				static_cast<uint32_t>(setLayoutBindings.size()));
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayouts.scene));

		// Set 1: Material data
		setLayoutBindings.clear();
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			0));
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayouts.material));

		// Setup pipeline layout
		std::array<VkDescriptorSetLayout, 2> setLayouts = { descriptorSetLayouts.scene, descriptorSetLayouts.material };
		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = vkTools::initializers::pipelineLayoutCreateInfo(setLayouts.data(), static_cast<uint32_t>(setLayouts.size()));

		// We will be using a push constant block to pass material properties to the fragment shaders
		VkPushConstantRange pushConstantRange = vkTools::initializers::pushConstantRange(
			VK_SHADER_STAGE_FRAGMENT_BIT, 
			sizeof(SceneMaterialProperites), 
			0);
		pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
		pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;

		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayout));

		// Material descriptor sets
		for (size_t i = 0; i < materials.size(); i++)
		{
			// Descriptor set
			VkDescriptorSetAllocateInfo allocInfo =
				vkTools::initializers::descriptorSetAllocateInfo(
					descriptorPool,
					&descriptorSetLayouts.material,
					1);

			VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &materials[i].descriptorSet));

			VkDescriptorImageInfo texDescriptor = 
				vkTools::initializers::descriptorImageInfo(
					materials[i].diffuse.sampler,
					materials[i].diffuse.view,
					VK_IMAGE_LAYOUT_GENERAL);

			std::vector<VkWriteDescriptorSet> writeDescriptorSets;

			// todo : only use image sampler descriptor set and use one scene ubo for matrices

			// Binding 0: Diffuse texture
			writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
				materials[i].descriptorSet,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				0,
				&texDescriptor));

			vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);
		}

		// Scene descriptor set
		VkDescriptorSetAllocateInfo allocInfo =
			vkTools::initializers::descriptorSetAllocateInfo(
				descriptorPool,
				&descriptorSetLayouts.scene,
				1);
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSetScene));

		std::vector<VkWriteDescriptorSet> writeDescriptorSets;
		// Binding 0 : Vertex shader uniform buffer
		writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
			descriptorSetScene,
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			0,
			&uniformBuffer.descriptor));

		vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);
	}

	// Load all meshes from the scene and generate the Vulkan resources
	// for rendering them
	void loadMeshes(VkCommandBuffer copyCmd)
	{
		meshes.resize(aScene->mNumMeshes);
		for (uint32_t i = 0; i < meshes.size(); i++)
		{
			aiMesh *aMesh = aScene->mMeshes[i];

			std::cout << "Mesh \"" << aMesh->mName.C_Str() << "\"" << std::endl;
			std::cout << "	Material: \"" << materials[aMesh->mMaterialIndex].name << "\"" << std::endl;
			std::cout << "	Faces: " << aMesh->mNumFaces << std::endl;

			meshes[i].material = &materials[aMesh->mMaterialIndex];

			// Vertices
			std::vector<Vertex> vertices;
			vertices.resize(aMesh->mNumVertices);

			bool hasUV = aMesh->HasTextureCoords(0);
			bool hasColor = aMesh->HasVertexColors(0);
			bool hasNormals = aMesh->HasNormals();

			for (uint32_t v = 0; v < aMesh->mNumVertices; v++)
			{
				vertices[v].pos = glm::make_vec3(&aMesh->mVertices[v].x);
				vertices[v].pos.y = -vertices[v].pos.y;
				vertices[v].uv = hasUV ? glm::make_vec2(&aMesh->mTextureCoords[0][v].x) : glm::vec2(0.0f);
				vertices[v].normal = hasNormals ? glm::make_vec3(&aMesh->mNormals[v].x) : glm::vec3(0.0f);
				vertices[v].normal.y = -vertices[v].normal.y;
				vertices[v].color = hasColor ? glm::make_vec3(&aMesh->mColors[0][v].r) : glm::vec3(1.0f);
			}

			// Indices
			std::vector<uint32_t> indices;
			meshes[i].indexCount = aMesh->mNumFaces * 3;
			indices.resize(aMesh->mNumFaces * 3);
			for (uint32_t f = 0; f < aMesh->mNumFaces; f++)
			{
				memcpy(&indices[f*3], &aMesh->mFaces[f].mIndices[0], sizeof(uint32_t) * 3);
			}

			// Create buffers
			// todo : only one memory allocation

			uint32_t vertexDataSize = vertices.size() * sizeof(Vertex);
			uint32_t indexDataSize = indices.size() * sizeof(uint32_t);

			VkMemoryAllocateInfo memAlloc = vkTools::initializers::memoryAllocateInfo();
			VkMemoryRequirements memReqs;

			struct
			{
				struct {
					VkDeviceMemory memory;
					VkBuffer buffer;
				} vBuffer;
				struct {
					VkDeviceMemory memory;
					VkBuffer buffer;
				} iBuffer;
			} staging;

			// Generate vertex buffer
			VkBufferCreateInfo vBufferInfo;
			void* data;

			// Staging buffer
			vBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vertexDataSize);
			VK_CHECK_RESULT(vkCreateBuffer(device, &vBufferInfo, nullptr, &staging.vBuffer.buffer));
			vkGetBufferMemoryRequirements(device, staging.vBuffer.buffer, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &staging.vBuffer.memory));
			VK_CHECK_RESULT(vkMapMemory(device, staging.vBuffer.memory, 0, VK_WHOLE_SIZE, 0, &data));
			memcpy(data, vertices.data(), vertexDataSize);
			vkUnmapMemory(device, staging.vBuffer.memory);
			VK_CHECK_RESULT(vkBindBufferMemory(device, staging.vBuffer.buffer, staging.vBuffer.memory, 0));

			// Target
			vBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, vertexDataSize);
			VK_CHECK_RESULT(vkCreateBuffer(device, &vBufferInfo, nullptr, &meshes[i].vertexBuffer));
			vkGetBufferMemoryRequirements(device, meshes[i].vertexBuffer, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &meshes[i].vertexMemory));
			VK_CHECK_RESULT(vkBindBufferMemory(device, meshes[i].vertexBuffer, meshes[i].vertexMemory, 0));

			// Generate index buffer
			VkBufferCreateInfo iBufferInfo;

			// Staging buffer
			iBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, indexDataSize);
			VK_CHECK_RESULT(vkCreateBuffer(device, &iBufferInfo, nullptr, &staging.iBuffer.buffer));
			vkGetBufferMemoryRequirements(device, staging.iBuffer.buffer, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &staging.iBuffer.memory));
			VK_CHECK_RESULT(vkMapMemory(device, staging.iBuffer.memory, 0, VK_WHOLE_SIZE, 0, &data));
			memcpy(data, indices.data(), indexDataSize);
			vkUnmapMemory(device, staging.iBuffer.memory);
			VK_CHECK_RESULT(vkBindBufferMemory(device, staging.iBuffer.buffer, staging.iBuffer.memory, 0));

			// Target
			iBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, indexDataSize);
			VK_CHECK_RESULT(vkCreateBuffer(device, &iBufferInfo, nullptr, &meshes[i].indexBuffer));
			vkGetBufferMemoryRequirements(device, meshes[i].indexBuffer, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &meshes[i].indexMemory));
			VK_CHECK_RESULT(vkBindBufferMemory(device, meshes[i].indexBuffer, meshes[i].indexMemory, 0));

			// Copy
			VkCommandBufferBeginInfo cmdBufInfo = vkTools::initializers::commandBufferBeginInfo();
			VK_CHECK_RESULT(vkBeginCommandBuffer(copyCmd, &cmdBufInfo));

			VkBufferCopy copyRegion = {};

			copyRegion.size = vertexDataSize;
			vkCmdCopyBuffer(
				copyCmd,
				staging.vBuffer.buffer,
				meshes[i].vertexBuffer,
				1,
				&copyRegion);

			copyRegion.size = indexDataSize;
			vkCmdCopyBuffer(
				copyCmd,
				staging.iBuffer.buffer,
				meshes[i].indexBuffer,
				1,
				&copyRegion);

			VK_CHECK_RESULT(vkEndCommandBuffer(copyCmd));

			VkSubmitInfo submitInfo = {};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submitInfo.commandBufferCount = 1;
			submitInfo.pCommandBuffers = &copyCmd;

			VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
			VK_CHECK_RESULT(vkQueueWaitIdle(queue));

			vkDestroyBuffer(device, staging.vBuffer.buffer, nullptr);
			vkFreeMemory(device, staging.vBuffer.memory, nullptr);
			vkDestroyBuffer(device, staging.iBuffer.buffer, nullptr);
			vkFreeMemory(device, staging.iBuffer.memory, nullptr);
		}
	}

public:
#if defined(__ANDROID__)
	AAssetManager* assetManager = nullptr;
#endif

	std::string assetPath = "";

	std::vector<SceneMaterial> materials;
	std::vector<SceneMesh> meshes;

	// Shared ubo containing matrices used by all
	// materials and meshes
	vkTools::UniformData uniformBuffer;
	struct {
		glm::mat4 projection;
		glm::mat4 view;
		glm::mat4 model;
		glm::vec4 lightPos = glm::vec4(1.25f, 8.35f, 0.0f, 0.0f);
	} uniformData;

	// Scene uses multiple pipelines
	struct {
		VkPipeline solid;
		VkPipeline blending;
		VkPipeline wireframe;
	} pipelines;

	// Shared pipeline layout
	VkPipelineLayout pipelineLayout;

	// For displaying only a single part of the scene
	bool renderSingleScenePart = false;
	uint32_t scenePartIndex = 0;

	Scene(VkDevice device, VkQueue queue, VkPhysicalDeviceMemoryProperties memprops, vkTools::VulkanTextureLoader *textureloader)
	{
		this->device = device;
		this->queue = queue;
		this->deviceMemProps = memprops;
		this->textureLoader = textureloader;

		// Prepare uniform buffer for global matrices
		VkMemoryRequirements memReqs;
		VkMemoryAllocateInfo memAlloc = vkTools::initializers::memoryAllocateInfo();
		VkBufferCreateInfo bufferCreateInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, sizeof(uniformData));
		VK_CHECK_RESULT(vkCreateBuffer(device, &bufferCreateInfo, nullptr, &uniformBuffer.buffer));
		vkGetBufferMemoryRequirements(device, uniformBuffer.buffer, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &uniformBuffer.memory));
		VK_CHECK_RESULT(vkBindBufferMemory(device, uniformBuffer.buffer, uniformBuffer.memory, 0));
		VK_CHECK_RESULT(vkMapMemory(device, uniformBuffer.memory, 0, sizeof(uniformData), 0, (void **)&uniformBuffer.mapped));
		uniformBuffer.descriptor.offset = 0;
		uniformBuffer.descriptor.buffer = uniformBuffer.buffer;
		uniformBuffer.descriptor.range = sizeof(uniformData);
	}

	~Scene()
	{
		for (auto mesh : meshes)
		{
			vkDestroyBuffer(device, mesh.vertexBuffer, nullptr);
			vkFreeMemory(device, mesh.vertexMemory, nullptr);
			vkDestroyBuffer(device, mesh.indexBuffer, nullptr);
			vkFreeMemory(device, mesh.indexMemory, nullptr);
		}
		for (auto material : materials)
		{
			textureLoader->destroyTexture(material.diffuse);
		}
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.material, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.scene, nullptr);
		vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		vkDestroyPipeline(device, pipelines.solid, nullptr);
		vkDestroyPipeline(device, pipelines.blending, nullptr);
		vkDestroyPipeline(device, pipelines.wireframe, nullptr);
		vkTools::destroyUniformData(device, &uniformBuffer);
	}

	void load(std::string filename, VkCommandBuffer copyCmd)
	{
		Assimp::Importer Importer;

		int flags = aiProcess_PreTransformVertices | aiProcess_Triangulate | aiProcess_GenNormals;

#if defined(__ANDROID__)
		AAsset* asset = AAssetManager_open(assetManager, filename.c_str(), AASSET_MODE_STREAMING);
		assert(asset);
		size_t size = AAsset_getLength(asset);
		assert(size > 0);
		void *meshData = malloc(size);
		AAsset_read(asset, meshData, size);
		AAsset_close(asset);
		aScene = Importer.ReadFileFromMemory(meshData, size, flags);
		free(meshData);
#else
		aScene = Importer.ReadFile(filename.c_str(), flags);
#endif
		if (aScene)
		{
			loadMaterials();
			loadMeshes(copyCmd);
		}
		else
		{
			printf("Error parsing '%s': '%s'\n", filename.c_str(), Importer.GetErrorString());
#if defined(__ANDROID__)
			LOGE("Error parsing '%s': '%s'", filename.c_str(), Importer.GetErrorString());
#endif
		}

	}

	// Renders the scene into an active command buffer
	// In a real world application we would do some visibility culling in here
	void render(VkCommandBuffer cmdBuffer, bool wireframe)
	{
		VkDeviceSize offsets[1] = { 0 };
		for (size_t i = 0; i < meshes.size(); i++)
		{
			if ((renderSingleScenePart) && (i != scenePartIndex))
				continue;

			//if (meshes[i].material->opacity == 0.0f)
			//	continue;

			// todo : per material pipelines
//			vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, *mesh.material->pipeline);

			// We will be using multiple descriptor sets for rendering
			// In GLSL the selection is done via the set and binding keywords
			// VS: layout (set = 0, binding = 0) uniform UBO;
			// FS: layout (set = 1, binding = 0) uniform sampler2D samplerColorMap;

			std::array<VkDescriptorSet, 2> descriptorSets;
			// Set 0: Scene descriptor set containing global matrices
			descriptorSets[0] = descriptorSetScene;
			// Set 1: Per-Material descriptor set containing bound images
			descriptorSets[1] = meshes[i].material->descriptorSet;

			vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, wireframe ? pipelines.wireframe : *meshes[i].material->pipeline);
			vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data(), 0, NULL);

			// Pass material properies via push constants
			vkCmdPushConstants(
				cmdBuffer,
				pipelineLayout,
				VK_SHADER_STAGE_FRAGMENT_BIT,
				0,
				sizeof(SceneMaterialProperites),
				&meshes[i].material->properties);

			vkCmdBindVertexBuffers(cmdBuffer, 0, 1, &meshes[i].vertexBuffer, offsets);
			vkCmdBindIndexBuffer(cmdBuffer, meshes[i].indexBuffer, 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexed(cmdBuffer, meshes[i].indexCount, 1, 0, 0, 0);
		}

		// Render transparent objects last

	}
};

class VulkanExample : public VulkanExampleBase
{
public:
	bool wireframe = false;
	bool attachLight = false;

	Scene *scene = nullptr;

	struct {
		VkPipelineVertexInputStateCreateInfo inputState;
		std::vector<VkVertexInputBindingDescription> bindingDescriptions;
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions;
	} vertices;

	VulkanExample() : VulkanExampleBase(ENABLE_VALIDATION)
	{
		rotationSpeed = 0.5f;
		enableTextOverlay = true;
		camera.type = Camera::CameraType::firstperson;
		camera.movementSpeed = 7.5f;
		camera.position = { 15.0f, -13.5f, 0.0f };
		camera.setRotation(glm::vec3(5.0f, 90.0f, 0.0f));
		camera.setPerspective(60.0f, (float)width / (float)height, 0.1f, 256.0f);
		title = "Vulkan Example - Scene rendering";
	}

	~VulkanExample()
	{
		delete(scene);
	}

	void reBuildCommandBuffers()
	{
		if (!checkCommandBuffers())
		{
			destroyCommandBuffers();
			createCommandBuffers();
		}
		buildCommandBuffers();
	}

	void buildCommandBuffers()
	{
		VkCommandBufferBeginInfo cmdBufInfo = vkTools::initializers::commandBufferBeginInfo();

		VkClearValue clearValues[2];
		clearValues[0].color = defaultClearColor;
		clearValues[0].color = { { 0.25f, 0.25f, 0.25f, 1.0f} };
		clearValues[1].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo renderPassBeginInfo = vkTools::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderPass = renderPass;
		renderPassBeginInfo.renderArea.offset.x = 0;
		renderPassBeginInfo.renderArea.offset.y = 0;
		renderPassBeginInfo.renderArea.extent.width = width;
		renderPassBeginInfo.renderArea.extent.height = height;
		renderPassBeginInfo.clearValueCount = 2;
		renderPassBeginInfo.pClearValues = clearValues;

		for (int32_t i = 0; i < drawCmdBuffers.size(); ++i)
		{
			renderPassBeginInfo.framebuffer = frameBuffers[i];

			VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));

			vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

			VkViewport viewport = vkTools::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
			vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

			VkRect2D scissor = vkTools::initializers::rect2D(width, height, 0, 0);
			vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

			scene->render(drawCmdBuffers[i], wireframe);

			vkCmdEndRenderPass(drawCmdBuffers[i]);

			VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
		}
	}

	void setupVertexDescriptions()
	{
		// Binding description
		vertices.bindingDescriptions.resize(1);
		vertices.bindingDescriptions[0] =
			vkTools::initializers::vertexInputBindingDescription(
				VERTEX_BUFFER_BIND_ID,
				sizeof(Vertex),
				VK_VERTEX_INPUT_RATE_VERTEX);

		// Attribute descriptions
		// Describes memory layout and shader positions
		vertices.attributeDescriptions.resize(4);
		// Location 0 : Position
		vertices.attributeDescriptions[0] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				0,
				VK_FORMAT_R32G32B32_SFLOAT,
				0);
		// Location 1 : Normal
		vertices.attributeDescriptions[1] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				1,
				VK_FORMAT_R32G32B32_SFLOAT,
				sizeof(float) * 3);
		// Location 2 : Texture coordinates
		vertices.attributeDescriptions[2] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				2,
				VK_FORMAT_R32G32_SFLOAT,
				sizeof(float) * 6);
		// Location 3 : Color
		vertices.attributeDescriptions[3] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				3,
				VK_FORMAT_R32G32B32_SFLOAT,
				sizeof(float) * 8);

		vertices.inputState = vkTools::initializers::pipelineVertexInputStateCreateInfo();
		vertices.inputState.vertexBindingDescriptionCount = vertices.bindingDescriptions.size();
		vertices.inputState.pVertexBindingDescriptions = vertices.bindingDescriptions.data();
		vertices.inputState.vertexAttributeDescriptionCount = vertices.attributeDescriptions.size();
		vertices.inputState.pVertexAttributeDescriptions = vertices.attributeDescriptions.data();
	}

	void preparePipelines()
	{
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState =
			vkTools::initializers::pipelineInputAssemblyStateCreateInfo(
				VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
				0,
				VK_FALSE);

		VkPipelineRasterizationStateCreateInfo rasterizationState =
			vkTools::initializers::pipelineRasterizationStateCreateInfo(
				VK_POLYGON_MODE_FILL,
				VK_CULL_MODE_BACK_BIT,
				VK_FRONT_FACE_COUNTER_CLOCKWISE,
				0);

		VkPipelineColorBlendAttachmentState blendAttachmentState =
			vkTools::initializers::pipelineColorBlendAttachmentState(
				0xf,
				VK_FALSE);

		VkPipelineColorBlendStateCreateInfo colorBlendState =
			vkTools::initializers::pipelineColorBlendStateCreateInfo(
				1,
				&blendAttachmentState);

		VkPipelineDepthStencilStateCreateInfo depthStencilState =
			vkTools::initializers::pipelineDepthStencilStateCreateInfo(
				VK_TRUE,
				VK_TRUE,
				VK_COMPARE_OP_LESS_OR_EQUAL);

		VkPipelineViewportStateCreateInfo viewportState =
			vkTools::initializers::pipelineViewportStateCreateInfo(1, 1, 0);

		VkPipelineMultisampleStateCreateInfo multisampleState =
			vkTools::initializers::pipelineMultisampleStateCreateInfo(
				VK_SAMPLE_COUNT_1_BIT,
				0);

		std::vector<VkDynamicState> dynamicStateEnables = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR
		};
		VkPipelineDynamicStateCreateInfo dynamicState =
			vkTools::initializers::pipelineDynamicStateCreateInfo(
				dynamicStateEnables.data(),
				dynamicStateEnables.size(),
				0);

		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

		// Solid rendering pipeline
		shaderStages[0] = loadShader(getAssetPath() + "shaders/scenerendering/scene.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getAssetPath() + "shaders/scenerendering/scene.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

		VkGraphicsPipelineCreateInfo pipelineCreateInfo =
			vkTools::initializers::pipelineCreateInfo(
				scene->pipelineLayout,
				renderPass,
				0);

		pipelineCreateInfo.pVertexInputState = &vertices.inputState;
		pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
		pipelineCreateInfo.pRasterizationState = &rasterizationState;
		pipelineCreateInfo.pColorBlendState = &colorBlendState;
		pipelineCreateInfo.pMultisampleState = &multisampleState;
		pipelineCreateInfo.pViewportState = &viewportState;
		pipelineCreateInfo.pDepthStencilState = &depthStencilState;
		pipelineCreateInfo.pDynamicState = &dynamicState;
		pipelineCreateInfo.stageCount = shaderStages.size();
		pipelineCreateInfo.pStages = shaderStages.data();

		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &scene->pipelines.solid));

		// Alpha blended pipeline
		rasterizationState.cullMode = VK_CULL_MODE_NONE;
		blendAttachmentState.blendEnable = VK_TRUE;
		blendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
		blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
		blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;

		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &scene->pipelines.blending));

		// Wire frame rendering pipeline
		rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
		blendAttachmentState.blendEnable = VK_FALSE;
		rasterizationState.polygonMode = VK_POLYGON_MODE_LINE;
		rasterizationState.lineWidth = 1.0f;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &scene->pipelines.wireframe));
	}

	void updateUniformBuffers()
	{
		if (attachLight)
		{
			scene->uniformData.lightPos = glm::vec4(-camera.position, 1.0f);
		}

		scene->uniformData.projection = camera.matrices.perspective;
		scene->uniformData.view = camera.matrices.view;
		scene->uniformData.model = glm::mat4();

		memcpy(scene->uniformBuffer.mapped, &scene->uniformData, sizeof(scene->uniformData));
	}

	void draw()
	{
		VulkanExampleBase::prepareFrame();

		// Command buffer to be sumitted to the queue
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];

		// Submit to queue
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));

		VulkanExampleBase::submitFrame();
	}

	void loadScene()
	{
		VkCommandBuffer copyCmd = VulkanExampleBase::createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
		scene = new Scene(device, queue, deviceMemoryProperties, textureLoader);

#if defined(__ANDROID__)
		scene->assetManager = androidApp->activity->assetManager;
#endif
		scene->assetPath = getAssetPath() + "models/sibenik/";
		scene->load(getAssetPath() + "models/sibenik/sibenik.dae", copyCmd);
		vkFreeCommandBuffers(device, cmdPool, 1, &copyCmd);
		updateUniformBuffers();
	}

	void prepare()
	{
		VulkanExampleBase::prepare();
		setupVertexDescriptions();
		loadScene();
		preparePipelines();
		buildCommandBuffers();
		prepared = true;
	}

	virtual void render()
	{
		if (!prepared)
			return;
		draw();
	}

	virtual void viewChanged()
	{
		updateUniformBuffers();
	}

	virtual void keyPressed(uint32_t keyCode)
	{
		switch (keyCode)
		{
		case 0x20:
		case GAMEPAD_BUTTON_A:
			wireframe = !wireframe;
			reBuildCommandBuffers();
			break;
		case 0x50:
			scene->renderSingleScenePart = !scene->renderSingleScenePart;
			reBuildCommandBuffers();
			updateTextOverlay();
			break;
		case 0x6B:
			scene->scenePartIndex = (scene->scenePartIndex < static_cast<uint32_t>(scene->meshes.size())) ? scene->scenePartIndex + 1 : 0;
			reBuildCommandBuffers();
			updateTextOverlay();
			break;
		case 0x6D:
			scene->scenePartIndex = (scene->scenePartIndex > 0) ? scene->scenePartIndex - 1 : static_cast<uint32_t>(scene->meshes.size()) - 1;
			updateTextOverlay();
			reBuildCommandBuffers();
			break;
		case 0x4C:
			attachLight = !attachLight;
			updateUniformBuffers();
			break;
		}
	}

	virtual void getOverlayText(VulkanTextOverlay *textOverlay)
	{
#if defined(__ANDROID__)
		textOverlay->addText("Press \"Button A\" to toggle wireframe", 5.0f, 85.0f, VulkanTextOverlay::alignLeft);
#else
		textOverlay->addText("Press \"w\" to toggle wireframe", 5.0f, 85.0f, VulkanTextOverlay::alignLeft);
		if ((scene) && (scene->renderSingleScenePart))
		{
			textOverlay->addText("Rendering mesh " + std::to_string(scene->scenePartIndex + 1) + " of " + std::to_string(static_cast<uint32_t>(scene->meshes.size())) + "(\"p\" to toggle)", 5.0f, 100.0f, VulkanTextOverlay::alignLeft);
		}
		else
		{
			textOverlay->addText("Rendering whole scene (\"p\" to toggle)", 5.0f, 100.0f, VulkanTextOverlay::alignLeft);
		}
#endif
	}
};

VulkanExample *vulkanExample;

#if defined(_WIN32)
LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (vulkanExample != NULL)
	{
		vulkanExample->handleMessages(hWnd, uMsg, wParam, lParam);
	}
	return (DefWindowProc(hWnd, uMsg, wParam, lParam));
}
#elif defined(__linux__) && !defined(__ANDROID__)
static void handleEvent(const xcb_generic_event_t *event)
{
	if (vulkanExample != NULL)
	{
		vulkanExample->handleEvent(event);
}
		}
#endif

// Main entry point
#if defined(_WIN32)
// Windows entry point
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow)
#elif defined(__ANDROID__)
// Android entry point
void android_main(android_app* state)
#elif defined(__linux__)
// Linux entry point
int main(const int argc, const char *argv[])
#endif
{
#if defined(__ANDROID__)
	// Removing this may cause the compiler to omit the main entry point 
	// which would make the application crash at start
	app_dummy();
#endif
	vulkanExample = new VulkanExample();
#if defined(_WIN32)
	vulkanExample->setupWindow(hInstance, WndProc);
#elif defined(__ANDROID__)
	// Attach vulkan example to global android application state
	state->userData = vulkanExample;
	state->onAppCmd = VulkanExample::handleAppCommand;
	state->onInputEvent = VulkanExample::handleAppInput;
	vulkanExample->androidApp = state;
#elif defined(__linux__)
	vulkanExample->setupWindow();
#endif
#if !defined(__ANDROID__)
	vulkanExample->initSwapchain();
	vulkanExample->prepare();
#endif
	vulkanExample->renderLoop();
	delete(vulkanExample);
#if !defined(__ANDROID__)
	return 0;
#endif
}