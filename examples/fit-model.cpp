/*
 * eos - A 3D Morphable Model fitting library written in modern C++11/14.
 *
 * File: examples/fit-model.cpp
 *
 * Copyright 2016 Patrik Huber
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "eos/core/Image.hpp"
#include "eos/core/image/opencv_interop.hpp"
#include "eos/core/Landmark.hpp"
#include "eos/core/LandmarkMapper.hpp"
#include "eos/core/read_pts_landmarks.hpp"
#include "eos/core/write_obj.hpp"
#include "eos/fitting/fitting.hpp"
#include "eos/morphablemodel/Blendshape.hpp"
#include "eos/morphablemodel/MorphableModel.hpp"
#include "eos/render/opencv/draw_utils.hpp"
#include "eos/render/texture_extraction.hpp"
#include "eos/cpp17/optional.hpp"

#include "Eigen/Core"

#include "boost/filesystem.hpp"
#include "boost/program_options.hpp"

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace eos;
namespace po = boost::program_options;
namespace fs = boost::filesystem;
using eos::core::Landmark;
using eos::core::LandmarkCollection;
using cv::Mat;
using std::cout;
using std::endl;
using std::string;
using std::vector;

/**
 * This app demonstrates estimation of the camera and fitting of the shape
 * model of a 3D Morphable Model from an ibug LFPW image with its landmarks.
 * In addition to fit-model-simple, this example uses blendshapes, contour-
 * fitting, and can iterate the fitting.
 *
 * 68 ibug landmarks are loaded from the .pts file and converted
 * to vertex indices using the LandmarkMapper.
 */





int main(int argc, char* argv[])
{
    string modelfile, imagefile, landmarksfile, mappingsfile, contourfile, edgetopologyfile, blendshapesfile,
        outputbasename;
    try
    {
        po::options_description desc("Allowed options");
        // clang-format off
        desc.add_options()
            ("help,h", "display the help message")
            ("model,m", po::value<string>(&modelfile)->required()->default_value("../share/sfm_shape_3448.bin"),
                "a Morphable Model stored as cereal BinaryArchive")
            ("image,i", po::value<string>(&imagefile)->required()->default_value("data/image_0010.png"),
                "an input image")
            ("landmarks,l", po::value<string>(&landmarksfile)->required()->default_value("data/image_0010.pts"),
                "2D landmarks for the image, in ibug .pts format")
            ("mapping,p", po::value<string>(&mappingsfile)->required()->default_value("../share/ibug_to_sfm.txt"),
                "landmark identifier to model vertex number mapping")
            ("model-contour,c", po::value<string>(&contourfile)->required()->default_value("../share/sfm_model_contours.json"),
                "file with model contour indices")
            ("edge-topology,e", po::value<string>(&edgetopologyfile)->required()->default_value("../share/sfm_3448_edge_topology.json"),
                "file with model's precomputed edge topology")
            ("blendshapes,b", po::value<string>(&blendshapesfile)->required()->default_value("../share/expression_blendshapes_3448.bin"),
                "file with blendshapes")
            ("output,o", po::value<string>(&outputbasename)->required()->default_value("out"),
                "basename for the output rendering and obj files");
        // clang-format on
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
        if (vm.count("help"))
        {
            cout << "Usage: fit-model [options]" << endl;
            cout << desc;
            return EXIT_SUCCESS;
        }
        po::notify(vm);
    } catch (const po::error& e)
    {
        cout << "Error while parsing command-line arguments: " << e.what() << endl;
        cout << "Use --help to display a list of options." << endl;
        return EXIT_FAILURE;
    }

    // Load the image, landmarks, LandmarkMapper and the Morphable Model:
    Mat image = cv::imread(imagefile);
    LandmarkCollection<Eigen::Vector2f> landmarks;
    try
    {
        landmarks = core::read_pts_landmarks(landmarksfile);
    } catch (const std::runtime_error& e)
    {
        cout << "Error reading the landmarks: " << e.what() << endl;
        return EXIT_FAILURE;
    }
    morphablemodel::MorphableModel morphable_model;
    try
    {
        morphable_model = morphablemodel::load_model(modelfile);
    } catch (const std::runtime_error& e)
    {
        cout << "Error loading the Morphable Model: " << e.what() << endl;
        return EXIT_FAILURE;
    }
    // The landmark mapper is used to map 2D landmark points (e.g. from the ibug scheme) to vertex ids:
    core::LandmarkMapper landmark_mapper;
    try
    {
        landmark_mapper = core::LandmarkMapper(mappingsfile);
    } catch (const std::exception& e)
    {
        cout << "Error loading the landmark mappings: " << e.what() << endl;
        return EXIT_FAILURE;
    }

    // The expression blendshapes:
    const vector<morphablemodel::Blendshape> blendshapes = morphablemodel::load_blendshapes(blendshapesfile);

    morphablemodel::MorphableModel morphable_model_with_expressions(
        morphable_model.get_shape_model(), blendshapes, morphable_model.get_color_model(), cpp17::nullopt,
        morphable_model.get_texture_coordinates());

    // These two are used to fit the front-facing contour to the ibug contour landmarks:
    const fitting::ModelContour model_contour =
        contourfile.empty() ? fitting::ModelContour() : fitting::ModelContour::load(contourfile);
    const fitting::ContourLandmarks ibug_contour = fitting::ContourLandmarks::load(mappingsfile);

    // The edge topology is used to speed up computation of the occluding face contour fitting:
    const morphablemodel::EdgeTopology edge_topology = morphablemodel::load_edge_topology(edgetopologyfile);

    // Draw the loaded landmarks:
    Mat outimg = image.clone();
    int count = 0;
    for (auto&& lm : landmarks)
    {

        // cv::rectangle(outimg, cv::Point2f(lm.coordinates[0] - 2.0f, lm.coordinates[1] - 2.0f),
        //               cv::Point2f(lm.coordinates[0] + 2.0f, lm.coordinates[1] + 2.0f), {255, 0, 0});
        count = count +  1;
        cv::circle(outimg, cv::Point(lm.coordinates[0], lm.coordinates[1]), 1, {255, 0, 0}); // Draw a larger circle
        cv::putText(outimg, std::to_string(count), cv::Point(lm.coordinates[0] + 3, lm.coordinates[1] - 3),
                    cv::FONT_HERSHEY_SIMPLEX, 0.3, {255, 0, 0}, 1, cv::LINE_AA);
    }

    // Fit the model, get back a mesh and the pose:
    core::Mesh mesh;
    fitting::RenderingParameters rendering_params;
    std::tie(mesh, rendering_params) = fitting::fit_shape_and_pose(
        morphable_model_with_expressions, landmarks, landmark_mapper, image.cols, image.rows, edge_topology,
        ibug_contour, model_contour, 5, cpp17::nullopt, 30.0f);


    /*   code */
    // std::cout << "Vertices:" << std::endl;
    // for (size_t i = 0; i < mesh.vertices.size(); ++i) {
    //     const Eigen::Vector3f& vertex = mesh.vertices[i];
    //     std::cout << "Vertex " << i << ": " << vertex.x() << " " << vertex.y() << " " << vertex.z() << std::endl;
    // }

    // // Print colors
    // std::cout << "Colors:" << std::endl;
    // for (const auto& color : mesh.colors) {
    //     std::cout << color.x() << " " << color.y() << " " << color.z() << std::endl;
    // }

    // // Print texcoords
    // std::cout << "Texture Coordinates:" << std::endl;
    // for (const auto& texcoord : mesh.texcoords) {
    //     std::cout << texcoord.x() << " " << texcoord.y() << std::endl;
    // }

    // // Print triangles
    // std::cout << "Triangles:" << std::endl;
    // for (const auto& triangle : mesh.tvi) {
    //     std::cout << triangle[0] << " " << triangle[1] << " " << triangle[2] << std::endl;
    // }




    // The 3D head pose can be recovered as follows - the function returns an Eigen::Vector3f with yaw, pitch,
    // and roll angles:
    const float yaw_angle = rendering_params.get_yaw_pitch_roll()[0];
    const float pitch_angle = rendering_params.get_yaw_pitch_roll()[1];
    const float roll_angle = rendering_params.get_yaw_pitch_roll()[2];
    cout<<"yaw_angle :"<<yaw_angle<<endl;
    cout<<"pitch_angle :"<<pitch_angle<<endl;
    cout<<"roll_angle :"<<roll_angle<<endl;

    // Extract the texture from the image using given mesh and camera parameters:
    const core::Image4u texturemap =
        render::extract_texture(mesh, rendering_params.get_modelview(), rendering_params.get_projection(),
                                render::ProjectionType::Orthographic, core::from_mat_with_alpha(image));

    // Draw the fitted mesh as wireframe, and save the image:
    render::draw_points_with_colored_indices(outimg, mesh, rendering_params.get_modelview(), rendering_params.get_projection(),
                           fitting::get_opencv_viewport(image.cols, image.rows));

    // render::draw_nearest_to_landmarks(outimg, mesh,landmarks ,rendering_params.get_modelview(), rendering_params.get_projection(),
    //                        fitting::get_opencv_viewport(image.cols, image.rows));
    

    fs::path outputfile = outputbasename + ".png";
    cv::imwrite(outputfile.string(), outimg);

    // Save the mesh as textured obj:
    outputfile.replace_extension(".obj");
    core::write_textured_obj(mesh, outputfile.string());

    // And save the texture map:
    outputfile.replace_extension(".texture.png");
    cv::imwrite(outputfile.string(), core::to_mat(texturemap));

    cout << "Finished fitting and wrote result mesh and texture to files with basename "
         << outputfile.stem().stem() << "." << endl;

    return EXIT_SUCCESS;
}
