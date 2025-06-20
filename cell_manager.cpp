#include "cell_manager.h"
#include "camera.h"
#include "config.h"
#include "ui_manager.h"
#include <iostream>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <vector>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <GLFW/glfw3.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/component_wise.hpp>

#include "genome.h"
#include "timer.h"

CellManager::CellManager()
{
    // Generate sphere mesh
    sphereMesh.generateSphere(12, 16, 1.0f); // Even lower poly count: 12x16 = 192 triangles
    sphereMesh.setupBuffers();

    initializeGPUBuffers();
    initializeSpatialGrid();

    // Initialize compute shaders
    physicsShader = new Shader("shaders/cell_physics_spatial.comp"); // Use spatial partitioning version
    updateShader = new Shader("shaders/cell_update.comp");
    internalUpdateShader = new Shader("shaders/cell_update_internal.comp");
    extractShader = new Shader("shaders/extract_instances.comp");
    cellCounterShader = new Shader("shaders/cell_counter.comp");
    cellAdditionShader = new Shader("shaders/apply_additions.comp");

    // Initialize spatial grid shaders
    gridClearShader = new Shader("shaders/grid_clear.comp");
    gridAssignShader = new Shader("shaders/grid_assign.comp");
    gridPrefixSumShader = new Shader("shaders/grid_prefix_sum.comp");
    gridInsertShader = new Shader("shaders/grid_insert.comp");
    
    // Initialize orientation gizmo shader and buffers
    gizmoShader = new Shader("shaders/gizmo.vert", "shaders/gizmo.frag");
    initializeGizmoBuffers();
    
    // Initialize ring gizmo shader and buffers
    ringGizmoShader = new Shader("shaders/ring_gizmo.vert", "shaders/ring_gizmo.frag");
    initializeRingGizmoBuffers();
}

CellManager::~CellManager()
{
    cleanup();
}

void CellManager::cleanup()
{
    // Clean up triple buffered cell buffers
    for (int i = 0; i < 3; i++)
    {
        if (cellBuffer[i] != 0)
        {
            glDeleteBuffers(1, &cellBuffer[i]);
            cellBuffer[i] = 0;
        }
    }
    if (instanceBuffer != 0)
    {
        glDeleteBuffers(1, &instanceBuffer);
        instanceBuffer = 0;
    }
    if (modeBuffer != 0)
    {
        glDeleteBuffers(1, &modeBuffer);
        modeBuffer = 0;
    }
    if (gpuCellCountBuffer != 0)
    {
        glDeleteBuffers(1, &gpuCellCountBuffer);
        gpuCellCountBuffer = 0;
    }
    if (stagingCellCountBuffer != 0)
    {
        glDeleteBuffers(1, &stagingCellCountBuffer);
        stagingCellCountBuffer = 0;
    }
    if (cellAdditionBuffer != 0)
    {
        glDeleteBuffers(1, &cellAdditionBuffer);
        cellAdditionBuffer = 0;
    }

    cleanupSpatialGrid();

    if (extractShader)
    {
        extractShader->destroy();
        delete extractShader;
        extractShader = nullptr;
    }
    if (physicsShader)
    {
        physicsShader->destroy();
        delete physicsShader;
        physicsShader = nullptr;
    }
    if (updateShader)
    {
        updateShader->destroy();
        delete updateShader;
        updateShader = nullptr;
    }

    // Cleanup spatial grid shaders
    if (gridClearShader)
    {
        gridClearShader->destroy();
        delete gridClearShader;
        gridClearShader = nullptr;
    }
    if (gridAssignShader)
    {
        gridAssignShader->destroy();
        delete gridAssignShader;
        gridAssignShader = nullptr;
    }
    if (gridPrefixSumShader)
    {
        gridPrefixSumShader->destroy();
        delete gridPrefixSumShader;
        gridPrefixSumShader = nullptr;
    }
    if (gridInsertShader)
    {
        gridInsertShader->destroy();
        delete gridInsertShader;
        gridInsertShader = nullptr;
    }
    
    // Cleanup gizmo resources
    cleanupGizmos();
    cleanupRingGizmos();

    sphereMesh.cleanup();
}

// Buffers

void CellManager::initializeGPUBuffers()
{    // Create triple buffered compute buffers for cell data
    for (int i = 0; i < 3; i++)
    {
        glCreateBuffers(1, &cellBuffer[i]);
        glNamedBufferData(
            cellBuffer[i],
            config::MAX_CELLS * sizeof(ComputeCell),
            nullptr,
            GL_DYNAMIC_COPY  // Used by both GPU compute and CPU read operations
        );

    }

    // Create instance buffer for rendering (contains position + radius)
    glCreateBuffers(1, &instanceBuffer);
    glNamedBufferData(
        instanceBuffer,
        config::MAX_CELLS * sizeof(glm::vec4) * 2, // 2 vec4s, one for pos and radius, the other for color
        nullptr,
        GL_DYNAMIC_DRAW
    );    // Create single buffered genome buffer
    glCreateBuffers(1, &modeBuffer);
    glNamedBufferData(modeBuffer,
        config::MAX_CELLS * sizeof(GPUMode),
        nullptr,
        GL_DYNAMIC_READ  // Written once by CPU, read frequently by both GPU and CPU
    );

    // A buffer that keeps track of how many cells there are in the simulation
    glCreateBuffers(1, &gpuCellCountBuffer);
    glNamedBufferStorage(
        gpuCellCountBuffer,
        sizeof(GLuint) * 2, // stores current cell count and pending cell count, can be expanded later to also include other counts
        nullptr,
        GL_DYNAMIC_STORAGE_BIT
    );

    glCreateBuffers(1, &stagingCellCountBuffer);
    glNamedBufferStorage(
        stagingCellCountBuffer,
        sizeof(GLuint) * 2,
        nullptr,
        GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT
    );
    mappedPtr = glMapNamedBufferRange(stagingCellCountBuffer, 0, sizeof(GLuint) * 2,
        GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    countPtr = static_cast<GLuint*>(mappedPtr);

    // Cell addition queue buffer
    glCreateBuffers(1, &cellAdditionBuffer);
    glNamedBufferData(cellAdditionBuffer,
        config::MAX_CELLS * sizeof(ComputeCell) / 2, // Worst case scenario
        nullptr,
        GL_DYNAMIC_DRAW
    );

    // Setup the sphere mesh to use our current instance buffer
    sphereMesh.setupInstanceBuffer(instanceBuffer);

    // Reserve CPU storage
    cpuCells.reserve(config::MAX_CELLS);
}

void CellManager::addCellsToGPUBuffer(const std::vector<ComputeCell> &cells)
{ // Prefer to not use this directly, use addCellToStagingBuffer instead
    int newCellCount = static_cast<int>(cells.size());

    std::cout << "Adding " << newCellCount << " cells to GPU buffer. Current cell count: " << cellCount << " -> " << cellCount + newCellCount << "\n";

    if (cellCount + newCellCount > config::MAX_CELLS)
    {
        std::cout << "Warning: Maximum cell count reached!\n";
        return;
    }


    TimerGPU gpuTimer("Adding Cells to GPU Buffers");
    // Update both cell buffers to keep them synchronized
    //for (int i = 0; i < 2; i++)
    //{
    //    glNamedBufferSubData(cellBuffer[i],
    //                         cellCount * sizeof(ComputeCell),
    //                         newCellCount * sizeof(ComputeCell),
    //                         cells.data());
    //}

    glNamedBufferSubData(cellAdditionBuffer,
        gpuPendingCellCount * sizeof(ComputeCell),
        newCellCount * sizeof(ComputeCell),
        cells.data());

    gpuPendingCellCount += newCellCount;
    glNamedBufferSubData(gpuCellCountBuffer, sizeof(int), sizeof(int), &gpuPendingCellCount);
    glCopyNamedBufferSubData(gpuCellCountBuffer, stagingCellCountBuffer, 0, 0, sizeof(GLuint) * 2);

    //cellCount += newCellCount;    // Cell count should be kept track of by the counter on the gpu
}

void CellManager::addCellToGPUBuffer(const ComputeCell &newCell)
{ // Prefer to not use this directly, use addCellToStagingBuffer instead
    addCellsToGPUBuffer({newCell});
}

void CellManager::addCellToStagingBuffer(const ComputeCell &newCell)
{
    if (cellCount + 1 > config::MAX_CELLS)
    {
        std::cout << "Warning: Maximum cell count reached!\n";
        return;
    }

    // Create a copy of the cell and enforce radius = 1.0f
    ComputeCell correctedCell = newCell;
    correctedCell.positionAndMass.w = 1.0f; // Force all cells to have radius of 1

    // Add to CPU storage only (no immediate GPU sync)
    cellStagingBuffer.push_back(correctedCell);
    cpuCells.push_back(correctedCell);
    cpuPendingCellCount++;
}

void CellManager::addStagedCellsToGPUBuffer()
{
    if (!cellStagingBuffer.empty()) {
        addCellsToGPUBuffer(cellStagingBuffer);
        cellStagingBuffer.clear(); // Clear after adding to GPU buffer
        cpuPendingCellCount = 0;      // Reset pending count
    }
}

void CellManager::addGenomeToBuffer(GenomeData& genomeData) const {
    int genomeBaseOffset = 0; // Later make it add to the end of the buffer
    int modeCount = static_cast<int>(genomeData.modes.size());

    std::vector<GPUMode> gpuModes;
    gpuModes.reserve(modeCount);

    for (size_t i = 0; i < modeCount; ++i) {
        const ModeSettings& mode = genomeData.modes[i];

        GPUMode gmode{};
        gmode.color = glm::vec4(mode.color, 0.0);
        gmode.splitInterval = mode.splitInterval;
        gmode.genomeOffset = genomeBaseOffset;

        // Convert degrees to radians
        gmode.splitOrientation = glm::radians(mode.parentSplitOrientation);

        // Store child mode indices
        gmode.childModes = glm::ivec2(mode.childA.modeNumber, mode.childB.modeNumber);

        gpuModes.push_back(gmode);
    }

    glNamedBufferSubData(
        modeBuffer,
        genomeBaseOffset,
        modeCount * sizeof(GPUMode),
        gpuModes.data()
    );
}

ComputeCell CellManager::getCellData(int index) const
{
    if (index >= 0 && index < cellCount)
    {
        return cpuCells[index];
    }
    return ComputeCell{}; // Return empty cell if index is invalid
}

void CellManager::updateCellData(int index, const ComputeCell &newData)
{
    if (index >= 0 && index < cellCount)
    {
        cpuCells[index] = newData;

        // Update selected cell cache if this is the selected cell
        if (selectedCell.isValid && selectedCell.cellIndex == index)
        {
            selectedCell.cellData = newData;
        }

        // Update the specific cell in both GPU buffers to keep them synchronized
        for (int i = 0; i < 2; i++)
        {
            glNamedBufferSubData(cellBuffer[i],
                                 index * sizeof(ComputeCell),
                                 sizeof(ComputeCell),
                                 &cpuCells[index]);
        }
    }
}

// Cell Update

void CellManager::updateCells(float deltaTime)
{
    glCopyNamedBufferSubData(gpuCellCountBuffer, stagingCellCountBuffer, 0, 0, sizeof(GLuint) * 2);

    cellCount = countPtr[0];
    gpuPendingCellCount = countPtr[1]; // This is effectively more of a bool than an int due to its horrific inaccuracy, but that's good enough for us
    int previousCellCount = cellCount;

    if (cpuPendingCellCount > 0)
    {
        addStagedCellsToGPUBuffer(); // Sync any pending cells to GPU
    }

    if (cellCount > 0) // Don't update cells if there are no cells to update
    {
	    // Update spatial grid before physics
    	updateSpatialGrid();

    	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    	// Run physics computation on GPU (reads from previous, writes to current)
    	runPhysicsCompute(deltaTime);

    	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        // Run position/velocity update on GPU (still working on current buffer)
        runUpdateCompute(deltaTime);

        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    	// Run cells' internal calculations (this creates new pending cells from mitosis)
    	runInternalUpdateCompute(deltaTime);

    	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }

    // Immediately apply any new cell divisions that occurred this frame
    glCopyNamedBufferSubData(gpuCellCountBuffer, stagingCellCountBuffer, 0, 0, sizeof(GLuint) * 2);
    gpuPendingCellCount = countPtr[1];
    
    //if (gpuPendingCellCount > 0)
    //{
    applyCellAdditions(); // Apply mitosis results immediately
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    //}
    
    // Update final cell count after all additions
    cellCount = countPtr[0];

    // Log if cells were added (split event occurred)
    if (cellCount > previousCellCount) {
        std::cout << "Split event occurred! Cell count increased from " << previousCellCount << " to " << cellCount << "\n";
    }

    // Swap buffers for next frame (current becomes previous, previous becomes current)
    rotateBuffers();
}

void CellManager::runCellCounter()  // This only count active cells in the gpu, not cells pending addition
{
    TimerGPU timer("Cell Counter");

    // Reset cell count to 0
    glClearNamedBufferData(
        gpuCellCountBuffer,        // Our cell counter buffer
        GL_R32UI,                  // Format (match the internal type)
        GL_RED_INTEGER,            // Format layout
        GL_UNSIGNED_INT,           // Data type
        nullptr                    // Null = fill with zero
    );

    cellCounterShader->use();

    // Set uniforms
    cellCounterShader->setInt("u_maxCells", config::MAX_CELLS);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, getCellReadBuffer());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gpuCellCountBuffer);

    // Dispatch compute shader
    GLuint numGroups = (config::MAX_CELLS + 63) / 64; // Round up division
    cellCounterShader->dispatch(numGroups, 1, 1);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); // Ensure writes are visible
    glCopyNamedBufferSubData(gpuCellCountBuffer, stagingCellCountBuffer, 0, 0, sizeof(GLuint));
}

void CellManager::renderCells(glm::vec2 resolution, Shader &cellShader, Camera &camera)
{
    if (cellCount == 0)
        return;

    // Safety check for zero-sized framebuffer (minimized window)
    if (resolution.x <= 0 || resolution.y <= 0)
    {
        return;
    }

    // Additional safety check for very small resolutions that might cause issues
    if (resolution.x < 1 || resolution.y < 1)
    {
        return;
    }
    try
    { // Use compute shader to efficiently extract instance data
	    {
		    TimerGPU timer("Instance extraction");

	    	extractShader->use();

	    	// Bind current buffers for compute shader (read from current cell buffer, write to current instance buffer)
	    	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, getCellReadBuffer());
	    	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, modeBuffer);
	    	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, instanceBuffer); // Dispatch extract compute shader
	    	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, gpuCellCountBuffer); // Bind GPU cell count buffer
	    	GLuint numGroups = (cellCount + 63) / 64;                                  // 64 threads per group
	    	extractShader->dispatch(numGroups, 1, 1);
	    }

        TimerGPU timer("Cell Rendering");

        // Use the sphere shader
        cellShader.use(); // Set up camera matrices (only calculate once per frame, not per cell)
        glm::mat4 view = camera.getViewMatrix();

        // Calculate aspect ratio with safety check
        float aspectRatio = resolution.x / resolution.y;
        if (aspectRatio <= 0.0f || !std::isfinite(aspectRatio))
        {
            // Use a default aspect ratio if calculation fails
            aspectRatio = 16.0f / 9.0f;
        }

        glm::mat4 projection = glm::perspective(glm::radians(45.0f),
                                                aspectRatio,
                                                0.1f, 1000.0f);
        // Set uniforms
        cellShader.setMat4("uProjection", projection);
        cellShader.setMat4("uView", view);
        cellShader.setVec3("uCameraPos", camera.getPosition());
        cellShader.setVec3("uLightDir", glm::vec3(1.0f, 1.0f, 1.0f));

        // Set selection highlighting uniforms
        if (selectedCell.isValid)
        {
            glm::vec3 selectedPos = glm::vec3(selectedCell.cellData.positionAndMass);
            float selectedRadius = selectedCell.cellData.getRadius();
            cellShader.setVec3("uSelectedCellPos", selectedPos);
            cellShader.setFloat("uSelectedCellRadius", selectedRadius);
        }
        else
        {
            cellShader.setVec3("uSelectedCellPos", glm::vec3(-9999.0f)); // Invalid position
            cellShader.setFloat("uSelectedCellRadius", 0.0f);
        }
        cellShader.setFloat("uTime", static_cast<float>(glfwGetTime())); // Enable depth testing for proper 3D rendering (don't clear here - already done in main loop)
        glEnable(GL_DEPTH_TEST);

        // Render instanced spheres
        sphereMesh.render(cellCount);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception in renderCells: " << e.what() << "\n";
    }
    catch (...)
    {
        std::cerr << "Unknown exception in renderCells\n";
    }
}

void CellManager::runPhysicsCompute(float deltaTime)
{
    TimerGPU timer("Cell Physics Compute");

    physicsShader->use();

    // Set uniforms

    // Pass dragged cell index to skip its physics
    int draggedIndex = (isDraggingCell && selectedCell.isValid) ? selectedCell.cellIndex : -1;
    physicsShader->setInt("u_draggedCellIndex", draggedIndex);

    // Set spatial grid uniforms
    physicsShader->setInt("u_gridResolution", config::GRID_RESOLUTION);
    physicsShader->setFloat("u_gridCellSize", config::GRID_CELL_SIZE);
    physicsShader->setFloat("u_worldSize", config::WORLD_SIZE);
    physicsShader->setInt("u_maxCellsPerGrid", config::MAX_CELLS_PER_GRID); // Bind buffers (read from previous buffer, write to current buffer for stable simulation)
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, getCellReadBuffer()); // Read from previous frame
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gridBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gridCountBuffer);

    // Also bind current buffer as output for physics results
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, getCellWriteBuffer()); // Write to current frame
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, gpuCellCountBuffer); // Bind GPU cell count buffer

    // Dispatch compute shader
    GLuint numGroups = (cellCount + 63) / 64; // Round up division
    physicsShader->dispatch(numGroups, 1, 1);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void CellManager::runUpdateCompute(float deltaTime)
{
    TimerGPU timer("Cell Update Compute");

	updateShader->use();

    // Set uniforms
    updateShader->setFloat("u_deltaTime", deltaTime);
    updateShader->setFloat("u_damping", 0.98f);

    // Pass dragged cell index to skip its position updates
    int draggedIndex = (isDraggingCell && selectedCell.isValid) ? selectedCell.cellIndex : -1;
    updateShader->setInt("u_draggedCellIndex", draggedIndex); // Bind current cell buffer for in-place updates
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, getCellWriteBuffer());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gpuCellCountBuffer); // Bind GPU cell count buffer

    // Dispatch compute shader
    GLuint numGroups = (cellCount + 63) / 64; // Round up division
    updateShader->dispatch(numGroups, 1, 1);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void CellManager::runInternalUpdateCompute(float deltaTime)
{
    TimerGPU timer("Cell Internal Update Compute");

    internalUpdateShader->use();

    // Set uniforms
    internalUpdateShader->setFloat("u_deltaTime", deltaTime);
    internalUpdateShader->setInt("u_maxCells", config::MAX_CELLS);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, modeBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, getCellWriteBuffer()); // Read from current buffer (has physics results)
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, cellAdditionBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, gpuCellCountBuffer);

    // Dispatch compute shader
    GLuint numGroups = (cellCount + 63) / 64; // Round up division
    internalUpdateShader->dispatch(numGroups, 1, 1);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void CellManager::applyCellAdditions()
{
    TimerGPU timer("Cell Additions");

    cellAdditionShader->use();

    // Set uniforms
    cellAdditionShader->setInt("u_maxCells", config::MAX_CELLS);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, cellAdditionBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, getCellReadBuffer());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, getCellWriteBuffer());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, gpuCellCountBuffer);

    // Dispatch compute shader
    GLuint numGroups = (config::MAX_CELLS / 2 + 63) / 64; // Horrific over-dispatch but it's better than under-dispatch and surprisingly doesn't hurt performance
    cellAdditionShader->dispatch(numGroups, 1, 1);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    GLuint zero = 0;    // This looks silly but I need a pointer to 0 to reset the pending cell counter
    glNamedBufferSubData(gpuCellCountBuffer, sizeof(GLuint), sizeof(GLuint), &zero); // offset = 4

    glCopyNamedBufferSubData(gpuCellCountBuffer, stagingCellCountBuffer, 0, 0, sizeof(GLuint) * 2);
}

void CellManager::resetSimulation()
{
    // Clear CPU-side data
    cpuCells.clear();
    cellStagingBuffer.clear();
    cellCount = 0;
    cpuPendingCellCount = 0;
    gpuPendingCellCount = 0;
    
    // CRITICAL FIX: Reset buffer rotation state for consistent keyframe restoration
    bufferRotation = 0;
    
    // Clear selection state
    clearSelection();
    
    // Clear GPU buffers by setting them to zero
    GLuint zero = 0;
    
    // Reset cell count buffers
    glNamedBufferSubData(gpuCellCountBuffer, 0, sizeof(GLuint), &zero); // cellCount = 0
    glNamedBufferSubData(gpuCellCountBuffer, sizeof(GLuint), sizeof(GLuint), &zero); // pendingCellCount = 0
    
    // Clear all cell buffers
    for (int i = 0; i < 3; i++)
    {
        glClearNamedBufferData(cellBuffer[i], GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    }

    glClearNamedBufferData(instanceBuffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    
    // Clear addition buffer
    glClearNamedBufferData(cellAdditionBuffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    
    // Clear spatial grid buffers
    glClearNamedBufferData(gridBuffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glClearNamedBufferData(gridCountBuffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glClearNamedBufferData(gridOffsetBuffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    
    // Sync the staging buffer
    glCopyNamedBufferSubData(gpuCellCountBuffer, stagingCellCountBuffer, 0, 0, sizeof(GLuint) * 2);
    
    std::cout << "Reset simulation (buffer rotation reset to 0)\n";
}

void CellManager::spawnCells(int count) // no longer functional, needs to be updated for the new cell struct
{
    TimerCPU cpuTimer("Spawning Cells");

    for (int i = 0; i < count && cellCount < config::MAX_CELLS; ++i)
    {
        // Random position within spawn radius
        float angle1 = static_cast<float>(rand()) / RAND_MAX * 2.0f * 3.14159f;
        float angle2 = static_cast<float>(rand()) / RAND_MAX * 3.14159f;
        float radius = static_cast<float>(rand()) / RAND_MAX * spawnRadius;

        glm::vec3 position = glm::vec3(
            radius * sin(angle2) * cos(angle1),
            radius * cos(angle2),
            radius * sin(angle2) * sin(angle1));

        // Random velocity
        glm::vec3 velocity = glm::vec3(
            (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 5.0f,
            (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 5.0f,
            (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 5.0f); // Random mass and fixed radius of 1
        //float mass = 1.0f + static_cast<float>(rand()) / RAND_MAX * 2.0f;
        //float cellRadius = 1.0f; // All cells have the same radius of 1

        ComputeCell newCell{};
        newCell.positionAndMass = glm::vec4(position, 1.);
        newCell.velocity = glm::vec4(velocity, 0.);
        newCell.acceleration = glm::vec4(0.0f); // Reset acceleration

        addCellToStagingBuffer(newCell);
    }
}

// Spatial partitioning
void CellManager::initializeSpatialGrid()
{
    // Create double buffered grid buffers to store cell indices

    glCreateBuffers(1, &gridBuffer);
    glNamedBufferData(gridBuffer,
                      config::TOTAL_GRID_CELLS * config::MAX_CELLS_PER_GRID * sizeof(GLuint),
                      nullptr, GL_DYNAMIC_DRAW);

    // Create double buffered grid count buffers to store number of cells per grid cell
    glCreateBuffers(1, &gridCountBuffer);
    glNamedBufferData(gridCountBuffer,
                      config::TOTAL_GRID_CELLS * sizeof(GLuint),
                      nullptr, GL_DYNAMIC_DRAW);

    // Create double buffered grid offset buffers for prefix sum calculations
    glCreateBuffers(1, &gridOffsetBuffer);
    glNamedBufferData(gridOffsetBuffer,
                      config::TOTAL_GRID_CELLS * sizeof(GLuint),
                      nullptr, GL_DYNAMIC_DRAW);

    std::cout << "Initialized double buffered spatial grid with " << config::TOTAL_GRID_CELLS
              << " grid cells (" << config::GRID_RESOLUTION << "^3)\n";
    std::cout << "Grid cell size: " << config::GRID_CELL_SIZE << "\n";
    std::cout << "Max cells per grid: " << config::MAX_CELLS_PER_GRID << "\n";
}

void CellManager::updateSpatialGrid()
{
    if (cellCount == 0)
        return;
    TimerGPU timer("Spatial Grid Update");

    // Step 1: Clear grid counts
    runGridClear();

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Step 2: Count cells per grid cell
    runGridAssign();

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Step 3: Calculate prefix sum for offsets
    runGridPrefixSum();

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Step 4: Insert cells into grid
    runGridInsert();

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void CellManager::cleanupSpatialGrid()
{
    // Clean up double buffered spatial grid buffers

    if (gridBuffer != 0)
    {
        glDeleteBuffers(1, &gridBuffer);
        gridBuffer = 0;
    }
    if (gridCountBuffer != 0)
    {
        glDeleteBuffers(1, &gridCountBuffer);
        gridCountBuffer = 0;
    }
    if (gridOffsetBuffer != 0)
    {
        glDeleteBuffers(1, &gridOffsetBuffer);
        gridOffsetBuffer = 0;
    }
}

void CellManager::runGridClear()
{
    gridClearShader->use();

    gridClearShader->setInt("u_totalGridCells", config::TOTAL_GRID_CELLS);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gridCountBuffer);

    GLuint numGroups = (config::TOTAL_GRID_CELLS + 63) / 64;
    gridClearShader->dispatch(numGroups, 1, 1);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void CellManager::runGridAssign()
{
    gridAssignShader->use();

    gridAssignShader->setInt("u_gridResolution", config::GRID_RESOLUTION);
    gridAssignShader->setFloat("u_gridCellSize", config::GRID_CELL_SIZE);
    gridAssignShader->setFloat("u_worldSize", config::WORLD_SIZE);

    // Use previous buffer for spatial grid to match physics compute input
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, getCellReadBuffer());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gridCountBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gpuCellCountBuffer); // Bind GPU cell count buffer

    GLuint numGroups = (cellCount + 63) / 64;
    gridAssignShader->dispatch(numGroups, 1, 1);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void CellManager::runGridPrefixSum()
{
    gridPrefixSumShader->use();

    gridPrefixSumShader->setInt("u_totalGridCells", config::TOTAL_GRID_CELLS);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gridCountBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gridOffsetBuffer);

    GLuint numGroups = (config::TOTAL_GRID_CELLS + 63) / 64;
    gridPrefixSumShader->dispatch(numGroups, 1, 1);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void CellManager::runGridInsert()
{
    gridInsertShader->use();    gridInsertShader->setInt("u_gridResolution", config::GRID_RESOLUTION);
    gridInsertShader->setFloat("u_gridCellSize", config::GRID_CELL_SIZE);
    gridInsertShader->setFloat("u_worldSize", config::WORLD_SIZE);
    gridInsertShader->setInt("u_maxCellsPerGrid", config::MAX_CELLS_PER_GRID); // Use previous buffer for spatial grid to match physics compute input
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, getCellReadBuffer());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gridBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gridOffsetBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, gridCountBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, gpuCellCountBuffer); // Bind GPU cell count buffer

    GLuint numGroups = (cellCount + 63) / 64;
    gridInsertShader->dispatch(numGroups, 1, 1);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// Cell selection and interaction implementation // todo: REWRITE FOR GPU ONLY
void CellManager::handleMouseInput(const glm::vec2 &mousePos, const glm::vec2 &screenSize,
                                   const Camera &camera, bool isMousePressed, bool isMouseDown,
                                   float scrollDelta)
{
    // Safety check for invalid screen size (minimized window)
    if (screenSize.x <= 0 || screenSize.y <= 0)
    {
        return;
    }

    // Handle scroll wheel to adjust drag distance when cell is selected
    if (selectedCell.isValid && scrollDelta != 0.0f)
    {
        float scrollSensitivity = 2.0f;
        selectedCell.dragDistance += scrollDelta * scrollSensitivity;
        selectedCell.dragDistance = glm::clamp(selectedCell.dragDistance, 1.0f, 100.0f);

        // If we're dragging, update the cell position to maintain the new distance
        if (isDraggingCell)
        {
            glm::vec3 rayDirection = calculateMouseRay(mousePos, screenSize, camera);
            glm::vec3 newWorldPos = camera.getPosition() + rayDirection * selectedCell.dragDistance;
            dragSelectedCell(newWorldPos);
        }
    }
    if (isMousePressed && !isDraggingCell)
    {
        // Sync current cell positions from GPU before attempting selection
        syncCellPositionsFromGPU();

        // Start new selection with improved raycasting
        glm::vec3 rayOrigin = camera.getPosition();
        glm::vec3 rayDirection = calculateMouseRay(mousePos, screenSize, camera);
        // Debug: Print mouse coordinates and ray info (reduced logging)
        std::cout << "Mouse click at (" << mousePos.x << ", " << mousePos.y << ")\n";

        int selectedIndex = selectCellAtPosition(rayOrigin, rayDirection);
        if (selectedIndex >= 0)
        {
            selectedCell.cellIndex = selectedIndex;
            selectedCell.cellData = cpuCells[selectedIndex];
            selectedCell.isValid = true;

            // Calculate the distance from camera to the selected cell
            glm::vec3 cellPosition = glm::vec3(selectedCell.cellData.positionAndMass);
            selectedCell.dragDistance = glm::distance(rayOrigin, cellPosition);

            // Calculate drag offset for smooth dragging
            glm::vec3 mouseWorldPos = rayOrigin + rayDirection * selectedCell.dragDistance;
            selectedCell.dragOffset = cellPosition - mouseWorldPos;

            isDraggingCell = true;

            // Debug output
            std::cout << "Selected cell " << selectedIndex << " at distance " << selectedCell.dragDistance << "\n";
        }
        else
        {
            clearSelection();
        }
    }

    if (isDraggingCell && isMouseDown && selectedCell.isValid)
    {
        // Continue dragging at the maintained distance
        glm::vec3 rayDirection = calculateMouseRay(mousePos, screenSize, camera);
        glm::vec3 newWorldPos = camera.getPosition() + rayDirection * selectedCell.dragDistance;
        dragSelectedCell(newWorldPos + selectedCell.dragOffset);
    }
    if (!isMouseDown)
    {
        if (isDraggingCell)
        {
            endDrag();
        }
    }
}

// todo: REWRITE FOR GPU ONLY
int CellManager::selectCellAtPosition(const glm::vec3 &rayOrigin, const glm::vec3 &rayDirection)
{
    float closestDistance = FLT_MAX;
    int closestCellIndex = -1;
    int intersectionCount = 0;

    // Debug output for raycasting
    std::cout << "Testing " << cellCount << " cells for intersection..." << std::endl;

    for (int i = 0; i < cellCount; i++)
    {
        glm::vec3 cellPosition = glm::vec3(cpuCells[i].positionAndMass);
        float cellRadius = cpuCells[i].getRadius();

        float intersectionDistance;
        if (raySphereIntersection(rayOrigin, rayDirection, cellPosition, cellRadius, intersectionDistance))
        {
            intersectionCount++;
            std::cout << "Cell " << i << " at (" << cellPosition.x << ", " << cellPosition.y << ", " << cellPosition.z
                      << ") radius " << cellRadius << " intersected at distance " << intersectionDistance << std::endl;

            if (intersectionDistance < closestDistance && intersectionDistance > 0)
            {
                closestDistance = intersectionDistance;
                closestCellIndex = i;
            }
        }
    }

    std::cout << "Found " << intersectionCount << " intersections total" << std::endl;
    if (closestCellIndex >= 0)
    {
        std::cout << "Selected closest cell " << closestCellIndex << " at distance " << closestDistance << std::endl;
    }
    else
    {
        std::cout << "No valid cell intersections found" << std::endl;
    }

    return closestCellIndex;
}

// todo: REWRITE FOR GPU ONLY
void CellManager::dragSelectedCell(const glm::vec3 &newWorldPosition)
{
    if (!selectedCell.isValid)
        return;

    // Update CPU cell data
    cpuCells[selectedCell.cellIndex].positionAndMass.x = newWorldPosition.x;
    cpuCells[selectedCell.cellIndex].positionAndMass.y = newWorldPosition.y;
    cpuCells[selectedCell.cellIndex].positionAndMass.z = newWorldPosition.z;

    // Clear velocity when dragging to prevent conflicts with physics
    cpuCells[selectedCell.cellIndex].velocity.x = 0.0f;
    cpuCells[selectedCell.cellIndex].velocity.y = 0.0f;
    cpuCells[selectedCell.cellIndex].velocity.z = 0.0f; // Update cached selected cell data
    selectedCell.cellData = cpuCells[selectedCell.cellIndex];

    // Update GPU buffers immediately to ensure compute shaders see the new position
    for (int i = 0; i < 3; i++)
    {
        glNamedBufferSubData(cellBuffer[i],
                             selectedCell.cellIndex * sizeof(ComputeCell),
                             sizeof(ComputeCell),
                             &cpuCells[selectedCell.cellIndex]);
    }
}

void CellManager::clearSelection()
{
    selectedCell.cellIndex = -1;
    selectedCell.isValid = false;
    isDraggingCell = false;
}

void CellManager::endDrag()
{
    if (isDraggingCell && selectedCell.isValid)
    {
        // Reset velocity to zero when ending drag to prevent sudden jumps
        cpuCells[selectedCell.cellIndex].velocity.x = 0.0f;
        cpuCells[selectedCell.cellIndex].velocity.y = 0.0f;
        cpuCells[selectedCell.cellIndex].velocity.z = 0.0f; // Update the GPU buffers with the final state
        for (int i = 0; i < 3; i++)
        {
            glNamedBufferSubData(cellBuffer[i],
                                 selectedCell.cellIndex * sizeof(ComputeCell),
                                 sizeof(ComputeCell),
                                 &cpuCells[selectedCell.cellIndex]);
        }
    }

    isDraggingCell = false;
}

void CellManager::syncCellPositionsFromGPU()
{ // This will fail if the CPU buffer has the wrong size, which will happen once cell division is implemented so i will have to rewrite this
    if (cellCount == 0)
        return;

    // CRITICAL FIX: Ensure all GPU operations are complete before mapping buffer
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glFinish(); // Wait for GPU to be completely idle

    // Use glMapBuffer for efficient GPU->CPU data transfer from current buffer
    ComputeCell *gpuData = static_cast<ComputeCell *>(glMapNamedBuffer(getCellReadBuffer(), GL_READ_ONLY));

    if (gpuData)
    {
        cpuCells.reserve(cellCount);
        // Copy only the position data from GPU to CPU (don't overwrite velocity/mass as those might be needed)
        for (int i = 0; i < cellCount; i++)
        {
            // Only sync position and radius, preserve velocity and mass from CPU
            //cpuCells[i].positionAndMass = gpuData[i].positionAndMass;
            if (i < cpuCells.size())
            {
                cpuCells[i] = gpuData[i]; // im syncing all of the data which is quite wasteful
            } else
            {
                cpuCells.push_back(gpuData[i]);
            }
            // if youre reading this and want to optimise this function please i beg of you
            // move cell selection to the gpu and only return the data of the selected cell
        }

        glUnmapNamedBuffer(getCellReadBuffer());

        std::cout << "Synced " << cellCount << " cell positions from GPU" << std::endl;
    }
    else
    {
        std::cerr << "Failed to map GPU buffer for readback" << std::endl;
    }
}

// todo: REWRITE FOR GPU ONLY
glm::vec3 CellManager::calculateMouseRay(const glm::vec2 &mousePos, const glm::vec2 &screenSize,
                                         const Camera &camera)
{
    // Safety check for zero screen size
    if (screenSize.x <= 0 || screenSize.y <= 0)
    {
        return camera.getFront(); // Return camera forward direction as fallback
    }

    // Convert screen coordinates to normalized device coordinates (-1 to 1)
    // Screen coordinates: (0,0) is top-left, (width,height) is bottom-right
    // NDC coordinates: (-1,-1) is bottom-left, (1,1) is top-right
    float x = (2.0f * mousePos.x) / screenSize.x - 1.0f;
    float y = 1.0f - (2.0f * mousePos.y) / screenSize.y; // Convert from screen Y (top-down) to NDC Y (bottom-up)

    // Create projection matrix (matching the one used in rendering)
    float aspectRatio = screenSize.x / screenSize.y;
    if (aspectRatio <= 0.0f || !std::isfinite(aspectRatio))
    {
        return camera.getFront(); // Return camera forward direction as fallback
    }

    glm::mat4 projection = glm::perspective(glm::radians(45.0f),
                                            aspectRatio,
                                            0.1f, 1000.0f);

    // Create view matrix
    glm::mat4 view = camera.getViewMatrix();

    // Calculate inverse view-projection matrix with error checking
    glm::mat4 viewProjection = projection * view;
    glm::mat4 inverseVP;

    // Check if the matrix is invertible
    float determinant = glm::determinant(viewProjection);
    if (abs(determinant) < 1e-6f)
    {
        return camera.getFront(); // Return camera forward direction as fallback
    }

    inverseVP = glm::inverse(viewProjection);

    // Create normalized device coordinate points for near and far planes
    glm::vec4 rayClipNear = glm::vec4(x, y, -1.0f, 1.0f);
    glm::vec4 rayClipFar = glm::vec4(x, y, 1.0f, 1.0f);

    // Transform to world space
    glm::vec4 rayWorldNear = inverseVP * rayClipNear;
    glm::vec4 rayWorldFar = inverseVP * rayClipFar;

    // Convert from homogeneous coordinates with safety checks
    if (abs(rayWorldNear.w) < 1e-6f || abs(rayWorldFar.w) < 1e-6f)
    {
        return camera.getFront(); // Return camera forward direction as fallback
    }

    rayWorldNear /= rayWorldNear.w;
    rayWorldFar /= rayWorldFar.w;

    // Calculate ray direction
    glm::vec3 rayDirection = glm::vec3(rayWorldFar) - glm::vec3(rayWorldNear);

    // Check if the direction is valid and normalize
    if (glm::length(rayDirection) < 1e-6f)
    {
        return camera.getFront(); // Return camera forward direction as fallback
    }

    rayDirection = glm::normalize(rayDirection);

    // Final validation
    if (!std::isfinite(rayDirection.x) || !std::isfinite(rayDirection.y) || !std::isfinite(rayDirection.z))
    {
        return camera.getFront(); // Return camera forward direction as fallback
    }

    return rayDirection;
}

// todo: REWRITE FOR GPU ONLY
bool CellManager::raySphereIntersection(const glm::vec3 &rayOrigin, const glm::vec3 &rayDirection,
                                        const glm::vec3 &sphereCenter, float sphereRadius, float &distance)
{
    glm::vec3 oc = rayOrigin - sphereCenter;
    float a = glm::dot(rayDirection, rayDirection);
    float b = 2.0f * glm::dot(oc, rayDirection);
    float c = glm::dot(oc, oc) - sphereRadius * sphereRadius;

    float discriminant = b * b - 4 * a * c;

    if (discriminant < 0)
    {
        return false; // No intersection
    }

    float sqrtDiscriminant = sqrt(discriminant);

    // Calculate both possible intersection points
    float t1 = (-b - sqrtDiscriminant) / (2.0f * a);
    float t2 = (-b + sqrtDiscriminant) / (2.0f * a);

    // Use the closest positive intersection (in front of the ray origin)
    if (t1 > 0.001f)
    { // Small epsilon to avoid self-intersection
        distance = t1;
        return true;
    }
    else if (t2 > 0.001f)
    {
        distance = t2;
        return true;
    }

    return false; // Both intersections are behind the ray origin or too close
}

// ===========================
// ORIENTATION GIZMO RENDERING
// ===========================

void CellManager::initializeGizmoBuffers()
{
    // Create VAO for gizmo lines
    glGenVertexArrays(1, &gizmoVAO);
    glBindVertexArray(gizmoVAO);
    
    // Create basic line geometry (will be transformed per cell)
    // Each gizmo consists of 3 lines: Forward (red), Up (green), Right (blue)
    // Each line goes from center to direction
    float gizmoLength = 1.5f;
    
    float gizmoLines[] = {
        // Forward direction (red) - positive Z in local space
        0.0f, 0.0f, 0.0f,    1.0f, 0.0f, 0.0f,  // start, color
        0.0f, 0.0f, gizmoLength,    1.0f, 0.0f, 0.0f,  // end, color
        
        // Up direction (green) - positive Y in local space
        0.0f, 0.0f, 0.0f,    0.0f, 1.0f, 0.0f,  // start, color
        0.0f, gizmoLength, 0.0f,    0.0f, 1.0f, 0.0f,  // end, color
        
        // Right direction (blue) - positive X in local space
        0.0f, 0.0f, 0.0f,    0.0f, 0.0f, 1.0f,  // start, color
        gizmoLength, 0.0f, 0.0f,    0.0f, 0.0f, 1.0f,  // end, color
    };
    
    // Create and bind VBO for line data
    glGenBuffers(1, &gizmoVBO);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(gizmoLines), gizmoLines, GL_STATIC_DRAW);
    
    // Set up vertex attributes
    // Position attribute (location 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Color attribute (location 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
}

void CellManager::updateGizmoData()
{
    // For now, gizmos will be drawn using instanced rendering with position and orientation data
    // The orientation will be extracted from cell data in the rendering method
}

void CellManager::renderOrientationGizmos(glm::vec2 resolution, const Camera &camera, const UIManager &uiManager)
{
    if (!uiManager.getShowOrientationGizmos() || cellCount == 0)
        return;
        
    if (gizmoVAO == 0 || !gizmoShader)
        return;
    
    // Safety check for zero-sized framebuffer (minimized window)
    if (resolution.x <= 0 || resolution.y <= 0)
        return;
    
    // Enable line rendering
    glEnable(GL_LINE_SMOOTH);
    glLineWidth(3.0f);
    
    gizmoShader->use();
    
    // Set camera matrices (use same projection as main rendering)
    glm::mat4 view = camera.getViewMatrix();
    float aspectRatio = resolution.x / resolution.y;
    glm::mat4 projection = glm::perspective(glm::radians(45.0f), 
                                          aspectRatio,
                                          0.1f, 1000.0f);
    
    // Set view and projection matrices once (they're the same for all gizmos)
    gizmoShader->setMat4("uProjection", projection);
    gizmoShader->setMat4("uView", view);
    
    glBindVertexArray(gizmoVAO);
    
    // Sync cell data from GPU to render gizmos
    // This is a temporary solution - ideally we'd use a compute shader to generate gizmo geometry
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, getCellReadBuffer());
    ComputeCell* cells = (ComputeCell*)glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_READ_ONLY);
    
    if (cells) {
        for (int i = 0; i < cellCount; i++) {
            const ComputeCell& cell = cells[i];
            glm::vec3 position = glm::vec3(cell.positionAndMass);
            
            // Handle orientation - if quaternion is zero/invalid, use identity
            glm::quat orientation = glm::quat(cell.orientation.w, cell.orientation.x, cell.orientation.y, cell.orientation.z);
            if (glm::length(orientation) < 0.1f) {
                orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // Identity quaternion (w, x, y, z)
            } else {
                orientation = glm::normalize(orientation);
            }
            
            // Calculate gizmo scale based on cell radius
            float radius = cell.getRadius();
            float gizmoScale = radius * 1.1f; // Gizmos twice the cell radius for visibility
            
            // Create transformation matrix for this cell's gizmo
            glm::mat4 modelMatrix = glm::mat4(1.0f);
            modelMatrix = glm::translate(modelMatrix, position);
            
            // Convert quaternion to rotation matrix manually
            // For now, use identity rotation since orientation might not be properly initialized
            // TODO: Properly convert quaternion to rotation matrix once orientation system is active
            // glm::mat4 rotationMatrix = glm::mat4_cast(orientation);
            glm::mat4 rotationMatrix = glm::mat4(1.0f); // Identity for now
            
            modelMatrix = modelMatrix * rotationMatrix;
            modelMatrix = glm::scale(modelMatrix, glm::vec3(gizmoScale));
            
            // Set model matrix for this gizmo
            gizmoShader->setMat4("uModel", modelMatrix);
            
            // Draw the 3 lines (6 vertices total)
            glDrawArrays(GL_LINES, 0, 6);
        }
        
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    
    glBindVertexArray(0);
    glLineWidth(2.0f);
    glDisable(GL_LINE_SMOOTH);
}

void CellManager::cleanupGizmos()
{
    if (gizmoShader) {
        gizmoShader->destroy();
        delete gizmoShader;
        gizmoShader = nullptr;
    }
    
    if (gizmoVAO != 0) {
        glDeleteVertexArrays(1, &gizmoVAO);
        gizmoVAO = 0;
    }
    
    if (gizmoVBO != 0) {
        glDeleteBuffers(1, &gizmoVBO);
        gizmoVBO = 0;
    }
    
    if (gizmoInstanceVBO != 0) {
        glDeleteBuffers(1, &gizmoInstanceVBO);
        gizmoInstanceVBO = 0;
    }
    
    if (ringGizmoShader) {
        ringGizmoShader->destroy();
        delete ringGizmoShader;
        ringGizmoShader = nullptr;
    }
    
    if (ringGizmoVAO != 0) {
        glDeleteVertexArrays(1, &ringGizmoVAO);
        ringGizmoVAO = 0;
    }
    
    if (ringGizmoVBO != 0) {
        glDeleteBuffers(1, &ringGizmoVBO);
        ringGizmoVBO = 0;
    }
}

// ===========================
// RING GIZMO RENDERING
// ===========================

void CellManager::initializeRingGizmoBuffers()
{
    // Create VAO for ring geometry
    glGenVertexArrays(1, &ringGizmoVAO);
    glBindVertexArray(ringGizmoVAO);
    
    // Create ring geometry - a flat ring made of triangles
    // Ring parameters
    const int segments = 32;  // Number of segments around the ring
    const float innerRadius = 0.4f;
    const float outerRadius = 0.45f;
    const float thickness = 0.001f; // Small thickness to make it visible from both sides
    
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    
    // Generate ring vertices (top and bottom faces)
    for (int i = 0; i <= segments; i++) {        float angle = (float)i / segments * 2.0f * (float)M_PI;
        float cosAngle = cos(angle);
        float sinAngle = sin(angle);
        
        // Top face vertices
        // Inner ring vertex (top)
        vertices.insert(vertices.end(), {
            innerRadius * cosAngle, thickness * 0.5f, innerRadius * sinAngle,  // position
            0.0f, 1.0f, 0.0f  // normal (pointing up)
        });
        
        // Outer ring vertex (top)
        vertices.insert(vertices.end(), {
            outerRadius * cosAngle, thickness * 0.5f, outerRadius * sinAngle,  // position
            0.0f, 1.0f, 0.0f  // normal (pointing up)
        });
        
        // Bottom face vertices  
        // Inner ring vertex (bottom)
        vertices.insert(vertices.end(), {
            innerRadius * cosAngle, -thickness * 0.5f, innerRadius * sinAngle,  // position
            0.0f, -1.0f, 0.0f  // normal (pointing down)
        });
        
        // Outer ring vertex (bottom)
        vertices.insert(vertices.end(), {
            outerRadius * cosAngle, -thickness * 0.5f, outerRadius * sinAngle,  // position
            0.0f, -1.0f, 0.0f  // normal (pointing down)
        });
    }
      // Generate indices for triangles
    for (int i = 0; i < segments; i++) {
        unsigned int base = (unsigned int)(i * 4);
        unsigned int next = (unsigned int)(((i + 1) % (segments + 1)) * 4);
        
        // Top face triangles
        indices.insert(indices.end(), {
            base, base + 1u, next,
            next, base + 1u, next + 1u
        });
        
        // Bottom face triangles
        indices.insert(indices.end(), {
            base + 2u, next + 2u, base + 3u,
            base + 3u, next + 2u, next + 3u
        });
    }
    
    // Create and bind VBO for ring data
    glGenBuffers(1, &ringGizmoVBO);
    glBindBuffer(GL_ARRAY_BUFFER, ringGizmoVBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    
    // Create and bind EBO for indices
    GLuint ringGizmoEBO;
    glGenBuffers(1, &ringGizmoEBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ringGizmoEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    
    // Set up vertex attributes
    // Position attribute (location 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Normal attribute (location 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
      // Store the number of indices for rendering
    ringGizmoIndexCount = static_cast<unsigned int>(indices.size());
    
    glBindVertexArray(0);
}

void CellManager::renderRingGizmos(glm::vec2 resolution, const Camera &camera, const UIManager &uiManager)
{
    if (!uiManager.getShowOrientationGizmos() || cellCount == 0)
        return;
        
    if (ringGizmoVAO == 0 || !ringGizmoShader)
        return;
    
    // Safety check for zero-sized framebuffer (minimized window)
    if (resolution.x <= 0 || resolution.y <= 0)
        return;
    
    // Enable blending for transparent rings
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    ringGizmoShader->use();
    
    // Set camera matrices (use same projection as main rendering)
    glm::mat4 view = camera.getViewMatrix();
    float aspectRatio = resolution.x / resolution.y;
    glm::mat4 projection = glm::perspective(glm::radians(45.0f), 
                                          aspectRatio,
                                          0.1f, 1000.0f);
    
    // Set view and projection matrices once (they're the same for all rings)
    ringGizmoShader->setMat4("uProjection", projection);
    ringGizmoShader->setMat4("uView", view);
    
    glBindVertexArray(ringGizmoVAO);
      // Access cell data from GPU to render rings
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, getCellReadBuffer());
    ComputeCell* cells = (ComputeCell*)glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_READ_ONLY);
    
    // Access mode data to get parent split orientation (map second buffer after first)
    GPUMode* modes = nullptr;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, modeBuffer);
    modes = (GPUMode*)glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_READ_ONLY);
    
    if (cells && modes) {
        for (int i = 0; i < cellCount; i++) {
            const ComputeCell& cell = cells[i];
            glm::vec3 position = glm::vec3(cell.positionAndMass);
            
            // Get the mode data for this cell to access parent split orientation
            int modeIndex = cell.modeIndex;
            if (modeIndex >= 0) { // Assuming we have valid mode data
                const GPUMode& mode = modes[modeIndex];
                  // Get parent split orientation (pitch and yaw in radians)
                float pitch = mode.splitOrientation.x;
                float yaw = mode.splitOrientation.y;
                
                // Calculate ring scale based on cell radius
                float radius = cell.getRadius();
                float ringScale = radius * 3.0f; // Ring slightly larger than cell for visibility
                
                // Create transformation matrix for this cell's ring
                glm::mat4 modelMatrix = glm::mat4(1.0f);
                modelMatrix = glm::translate(modelMatrix, position);
                  // Orient the ring to represent the splitting plane (perpendicular to split direction)
                // The ring should show where the cell will divide, so it's rotated 90 degrees from the split direction
                // First apply the yaw rotation
                modelMatrix = glm::rotate(modelMatrix, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                // Then apply pitch rotation + 90 degrees to make ring perpendicular to split direction
                // Negate pitch to correct rotation direction
                modelMatrix = glm::rotate(modelMatrix, -pitch + (float)M_PI * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
                
                modelMatrix = glm::scale(modelMatrix, glm::vec3(ringScale));
                
                // Set model matrix for this ring
                ringGizmoShader->setMat4("uModel", modelMatrix);
                
                // Draw the ring
                glDrawElements(GL_TRIANGLES, ringGizmoIndexCount, GL_UNSIGNED_INT, 0);
            }
        }
    }
    
    // Unmap buffers in reverse order
    if (modes) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, modeBuffer);
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    
    if (cells) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, getCellReadBuffer());
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void CellManager::cleanupRingGizmos()
{
    if (ringGizmoShader) {
        ringGizmoShader->destroy();
        delete ringGizmoShader;
        ringGizmoShader = nullptr;
    }
    
    if (ringGizmoVAO != 0) {
        glDeleteVertexArrays(1, &ringGizmoVAO);
        ringGizmoVAO = 0;
    }
    
    if (ringGizmoVBO != 0) {
        glDeleteBuffers(1, &ringGizmoVBO);
        ringGizmoVBO = 0;
    }
}
