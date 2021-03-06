#if defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
#define WINDOWS_LEAN_AND_MEAN // Please, not too much windows shenanigans
#endif

// Standard libs
#include <string>
#include <stdio.h>
// GLM for maths
#include <glm/glm.hpp>
#include <glm/gtx/string_cast.hpp>
// Trimesh for model importing
#include "TriMesh.h"
// TinyObj for alternative model importing
// #define TINYOBJLOADER_IMPLEMENTATION // define this in only *one* .cc
// #include "tiny_obj_loader.h"
#include "util_io.h"
#include "util_cuda.h"
#include "util_common.h"
#include "thrust/device_vector.h"
#include "thrust/host_vector.h"

// Forward declaration of CUDA calls
extern void voxelize(const voxinfo & v, float* triangle_data, unsigned int* vtable, bool useMallocManaged, bool morton_code);

using namespace std;

string version_number = "v0.2";
enum OutputFormat { output_binvox, output_morton};
char *OutputFormats[] = { "binvox file", "morton encoded blob" };

// Default options
string filename = "";
string filename_base = "";
OutputFormat outputformat = output_binvox;
unsigned int gridsize = 256;
bool useMallocManaged = false;

// Program data
// When we use managed memory, these are globally available pointers (HOST and DEVICE)
// When not, these are HOST-only pointers
unsigned int* vtable;
thrust::host_vector<float> trianglethrust_host;
thrust::device_vector<float> trianglethrust_device;

// Limitations
size_t GPU_global_mem;

void printHeader(){
	cout << "CUDA Voxelizer " << version_number << " by Jeroen Baert" << endl; 
	cout << "github.com/Forceflow/cuda_voxelizer - jeroen.baert@cs.kuleuven.be" << endl;
}

void printHelp(){
	cout << "Program options: " << endl;
	cout << " -f <path to model file: .ply, .obj, .3ds>" << endl;
	cout << " -s <voxelization grid size, power of 2: 8 -> 512, 1024, ... (default: 256)>" << endl << std::endl;
	cout << " -o <output format: binvox or morton (default: binvox)>" << endl << std::endl;
	cout << "Example: cuda_voxelizer -f /home/jeroen/bunny.ply -s 512" << endl;
}

// METHOD 1: Helper function to transfer triangles to automatically managed CUDA memory
void trianglesToGPU_managed(const trimesh::TriMesh *mesh, float** triangles) {
	size_t n_floats = sizeof(float) * 9 * (mesh->faces.size());
	fprintf(stdout, "Allocating %llu kB of CUDA-managed UNIFIED memory \n", (size_t)(n_floats / 1024.0f));
	checkCudaErrors(cudaMallocManaged((void**) triangles, n_floats)); // managed memory
	fprintf(stdout, "Copy %llu triangles to CUDA-managed UNIFIED memory \n", (size_t)(mesh->faces.size()));
	for (size_t i = 0; i < mesh->faces.size(); i++) {
		const trimesh::point &v0 = mesh->vertices[mesh->faces[i][0]];
		const trimesh::point &v1 = mesh->vertices[mesh->faces[i][1]];
		const trimesh::point &v2 = mesh->vertices[mesh->faces[i][2]];
		size_t j = i * 9;
		memcpy((triangles)+j, &v0, 3 * sizeof(float));
		memcpy((triangles)+j + 3, &v1, 3 * sizeof(float));
		memcpy((triangles)+j + 6, &v2, 3 * sizeof(float));
	}
}

// METHOD 2: Helper function to transfer triangles to old-style, self-managed CUDA memory
void trianglesToGPU(const trimesh::TriMesh *mesh, float** triangles){
	size_t n_floats = sizeof(float) * 9 * (mesh->faces.size());
	float* triangle_pointer;
	fprintf(stdout, "Allocating %llu kb of page-locked HOST memory \n", (size_t)(n_floats / 1024.0f));
	checkCudaErrors(cudaHostAlloc((void**)&triangle_pointer, n_floats, cudaHostAllocDefault)); // pinned memory to easily copy from
	fprintf(stdout, "Copy %llu triangles to page-locked HOST memory \n", (size_t)(mesh->faces.size()));
	for (size_t i = 0; i < mesh->faces.size(); i++){
		const trimesh::point &v0 = mesh->vertices[mesh->faces[i][0]];
		const trimesh::point &v1 = mesh->vertices[mesh->faces[i][1]];
		const trimesh::point &v2 = mesh->vertices[mesh->faces[i][2]];
		size_t j = i * 9;
		memcpy((triangle_pointer) + j, &v0, 3 * sizeof(float));
		memcpy((triangle_pointer) + j + 3, &v1, 3 * sizeof(float));
		memcpy((triangle_pointer) + j + 6, &v2, 3 * sizeof(float));
	}
	fprintf(stdout, "Allocating %llu kb of DEVICE memory \n", (size_t)(n_floats / 1024.0f));
	checkCudaErrors(cudaMalloc((void **) triangles, n_floats));
	fprintf(stdout, "Copy %llu triangles from page-locked HOST memory to DEVICE memory \n", (size_t)(mesh->faces.size()));
	checkCudaErrors(cudaMemcpy((void *) *triangles, (void*) triangle_pointer, n_floats, cudaMemcpyDefault));
}

// METHOD 3: Use a thrust vector
void trianglesToGPU_thrust(const trimesh::TriMesh *mesh, float** triangles) {
	// Fill host vector
	thrust::host_vector<float> trianglethrust_host;
	for (size_t i = 0; i < mesh->faces.size(); i++) {
		const trimesh::point &v0 = mesh->vertices[mesh->faces[i][0]];
		const trimesh::point &v1 = mesh->vertices[mesh->faces[i][1]];
		const trimesh::point &v2 = mesh->vertices[mesh->faces[i][2]];
		size_t j = i * 9;
		trianglethrust_host.push_back(v0[0]);
		trianglethrust_host.push_back(v0[1]);
		trianglethrust_host.push_back(v0[2]);
		trianglethrust_host.push_back(v1[0]);
		trianglethrust_host.push_back(v1[1]);
		trianglethrust_host.push_back(v1[2]);
		trianglethrust_host.push_back(v2[0]);
		trianglethrust_host.push_back(v2[1]);
		trianglethrust_host.push_back(v2[2]);
	}
	trianglethrust_device = trianglethrust_host;
	*triangles = (float*)thrust::raw_pointer_cast(&(trianglethrust_device[0]));
}

void parseProgramParameters(int argc, char* argv[]){
	if(argc<2){ // not enough arguments
		fprintf(stdout, "Not enough program parameters. \n \n");
		printHelp();
		exit(0);
	} 
	for (int i = 1; i < argc; i++) {
		if (string(argv[i]) == "-f") {
			filename = argv[i + 1];
			filename_base = filename.substr(0, filename.find_last_of("."));
			i++;
		} else if (string(argv[i]) == "-s") {
			gridsize = atoi(argv[i + 1]);
			i++;
		} else if (string(argv[i]) == "-o") {
			string output = (argv[i + 1]);
			transform(output.begin(), output.end(), output.begin(), ::tolower); // to lowercase
			if (output == "binvox"){
				outputformat = output_binvox;
			}
			else if (output == "morton"){
				outputformat = output_morton;
			}
			else {
				fprintf(stdout, "Unrecognized output format: %s, valid options are binvox (default) or morton \n", output);
				exit(0);
			}
		}
		else if (string(argv[i]) == "-m") {
			useMallocManaged = true;
		}
	}
	fprintf(stdout, "Filename: %s \n", filename.c_str());
	fprintf(stdout, "Grid size: %i \n", gridsize);
	fprintf(stdout, "Output format: %s \n", OutputFormats[outputformat]);
	fprintf(stdout, "Using CUDA Unified memory alloc: %s \n", useMallocManaged ? "Yes" : "No");
}

int main(int argc, char *argv[]) {
	printHeader();
	fprintf(stdout, "\n## PROGRAM PARAMETERS \n");
	parseProgramParameters(argc, argv);
	fprintf(stdout, "\n## CUDA INIT \n");
	checkCudaRequirements();

	fflush(stdout);
	trimesh::TriMesh::set_verbose(false);
#ifdef _DEBUG
	fprintf(stdout, "\n## MESH IMPORT \n");
	trimesh::TriMesh::set_verbose(true);
#endif
	trimesh::TriMesh *themesh = trimesh::TriMesh::read(filename.c_str());
	themesh->need_faces(); // Trimesh: Unpack (possible) triangle strips so we have faces
	themesh->need_bbox(); // Trimesh: Compute the bounding box

	fprintf(stdout, "\n## TRIANGLES TO GPU TRANSFER \n");
	fprintf(stdout, "Number of faces: %llu, faces table takes %llu kB \n", themesh->faces.size(), (size_t) (themesh->faces.size()*sizeof(trimesh::TriMesh::Face) / 1024.0f));
	fprintf(stdout, "Number of vertices: %llu, vertices table takes %llu kB \n", themesh->vertices.size(), (size_t) (themesh->vertices.size()*sizeof(trimesh::point) / 1024.0f));
	size_t size = sizeof(float) * 9 * (themesh->faces.size());
	float* triangles;

	//cudaEvent_t tGPUtime_start, tGPUtime_stop;
	//checkCudaErrors(cudaEventCreate(&tGPUtime_start));
	//checkCudaErrors(cudaEventCreate(&tGPUtime_stop));
	//checkCudaErrors(cudaEventRecord(tGPUtime_start, 0));
	
	if(useMallocManaged){ 
		trianglesToGPU_managed(themesh, &triangles);
	}
	else {
		trianglesToGPU_thrust(themesh, &triangles);
	}

	//float elapsedTime;
	//checkCudaErrors(cudaEventRecord(tGPUtime_stop, 0));
	//checkCudaErrors(cudaEventSynchronize(tGPUtime_stop));
	//checkCudaErrors(cudaEventElapsedTime(&elapsedTime, tGPUtime_start, tGPUtime_stop));
	//printf("Triangle to GPU:  %3.1f ms\n", elapsedTime);

	fprintf(stdout, "\n## VOXELISATION SETUP \n");
	AABox<glm::vec3> bbox_mesh(trimesh_to_glm(themesh->bbox.min), trimesh_to_glm(themesh->bbox.max)); // compute bbox around mesh
	voxinfo v(createMeshBBCube<glm::vec3>(bbox_mesh), gridsize, themesh->faces.size());
	v.print();
	size_t vtable_size = ((size_t)gridsize*gridsize*gridsize) / 8.0f;

	if (useMallocManaged) {
		fprintf(stdout, "Allocating %llu kB of CUDA-managed UNIFIED memory for voxel table \n", size_t(vtable_size / 1024.0f));
		checkCudaErrors(cudaMallocManaged((void **)&vtable, vtable_size));
	}
	else{
		// ALLOCATE MEMORY ON HOST
		fprintf(stdout, "Allocating %llu kB of page-locked HOST memory for voxel table \n", size_t(vtable_size / 1024.0f));
		checkCudaErrors(cudaHostAlloc((void **)&vtable, vtable_size, cudaHostAllocDefault));
	}
	fprintf(stdout, "\n## GPU VOXELISATION \n");
	voxelize(v, triangles, vtable, useMallocManaged, (outputformat == output_morton));

	if (outputformat == output_morton){
		fprintf(stdout, "\n## OUTPUT TO BINARY FILE \n");
		write_binary(vtable, vtable_size, filename);
	} else if (outputformat == output_binvox){
		fprintf(stdout, "\n## OUTPUT TO BINVOX FILE \n");
		write_binvox(vtable, gridsize, filename);
	}
}
