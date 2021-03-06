#ifdef USE_OPENCV
#include <opencv2/core/core.hpp>
#endif  // USE_OPENCV
#include <stdint.h>

#include <vector>

#include "caffe/data_transformer.hpp"
#include "caffe/layers/data_layer.hpp"
#include "caffe/util/benchmark.hpp"
// OpenPose: added
#include <chrono>
#include <stdexcept>
#include "caffe/util/io.hpp" // DecodeDatum, DecodeDatumNative
#include "caffe/openpose/getLine.hpp"
#include "caffe/openpose/layers/oPTripletLayer.hpp"
#include <opencv2/opencv.hpp>
#include <chrono>
#include <thread>
#include "caffe/caffe.hpp"
#include "caffe/blob.hpp"
#include <caffe/net.hpp>
// OpenPose: added end

#include <iostream>
using namespace std;

namespace caffe {

template <typename Dtype>
OPTripletLayer<Dtype>::OPTripletLayer(const LayerParameter& param) :
    BasePrefetchingDataLayer<Dtype>(param),
    offset_(),
    offsetSecond(), // OpenPose: added
    op_transform_param_(param.op_transform_param()) // OpenPose: added
{
    // LOAD THE TEXT FILE HERE
//    db_.reset(db::GetDB(param.data_param().backend()));
//    db_->Open(param.data_param().source(), db::READ);
//    cursor_.reset(db_->NewCursor());

    // OpenPose: added
    mOnes = 0;
    mTwos = 0;
    // Set up secondary DB
    if (!param.op_transform_param().source_secondary().empty())
    {
        secondDb = true;
        secondProbability = param.op_transform_param().prob_secondary();
        CHECK_GE(secondProbability, 0.f);
        CHECK_LE(secondProbability, 1.f);
        dbSecond.reset(db::GetDB(DataParameter_DB::DataParameter_DB_LMDB));
        dbSecond->Open(param.op_transform_param().source_secondary(), db::READ);
        cursorSecond.reset(dbSecond->NewCursor());
    }
    else
    {
        secondDb = false;
        secondProbability = 0.f;
    }
    // Set up negatives DB
    if (!param.op_transform_param().source_background().empty())
    {
        backgroundDb = true;
        dbBackground.reset(db::GetDB(DataParameter_DB::DataParameter_DB_LMDB));
        dbBackground->Open(param.op_transform_param().source_background(), db::READ);
        cursorBackground.reset(dbBackground->NewCursor());
    }
    else
        backgroundDb = false;
    // Timer
    mDuration = 0;
    mCounter = 0;
    // OpenPose: added end
}

template <typename Dtype>
OPTripletLayer<Dtype>::~OPTripletLayer()
{
    this->StopInternalThread();
}


template <typename Dtype>
void OPTripletLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top)
{
    debug_mode = this->layer_param_.op_transform_param().debug_mode();

    // If ID is wanted
    id_available = top.size() - 2;

    std::string train_source = this->layer_param().data_param().source();
    std::ifstream file(train_source + "train_info.txt");
    std::string str;
    while (std::getline(file, str))
    {
        std::vector<std::string> splitString;
        boost::split(splitString,str,boost::is_any_of(" "));
        int person_id = std::stoi(splitString[0]);
        std::string image_path = splitString[1];
        if(!reidData.count(person_id)) reidData[person_id] = std::vector<std::string>();
        reidData[person_id].emplace_back(train_source + image_path);
    }
    file.close();
    for (auto& kv : reidData) {
        reidKeys.emplace_back(kv.first);
    }
    if(!reidData.size()) throw std::runtime_error("Failed to load primary");

    // Secondary
    if(this->layer_param_.op_transform_param().model_secondary().size() && this->layer_param_.op_transform_param().secondary_mode() == 0){
        secondary_prob = this->layer_param_.op_transform_param().prob_secondary();

        std::string train_source = this->layer_param_.op_transform_param().model_secondary();
        std::ifstream file(train_source + "train_info.txt");
        std::string str;
        while (std::getline(file, str))
        {
            std::vector<std::string> splitString;
            boost::split(splitString,str,boost::is_any_of(" "));
            int person_id = std::stoi(splitString[0]);
            std::string image_path = splitString[1];
            if(!reidData_secondary.count(person_id)) reidData_secondary[person_id] = std::vector<std::string>();
            reidData_secondary[person_id].emplace_back(train_source + image_path);
        }
        file.close();
        for (auto& kv : reidData_secondary) {
            reidKeys_secondary.emplace_back(kv.first);
        }
        if(!reidData_secondary.size()) throw std::runtime_error("Failed to load secondary");
    }
    else if(this->layer_param_.op_transform_param().model_secondary().size() && this->layer_param_.op_transform_param().secondary_mode() == 1){
        secondary_prob = this->layer_param_.op_transform_param().prob_secondary();
        std::string jsonPath = this->layer_param_.op_transform_param().model_secondary();

        Json::Value root;
        std::ifstream file(jsonPath);
        file >> root;

        if(root.size() == 0) throw std::runtime_error("Failed to load JSON");

        for(int i=0; i<root.size(); i++){
            TVideo tVideo;
            for(int j=0; j<root[i].size(); j++){
                TFrame tFrame;
                tFrame.imagePath = root[i][j]["image_path_full"].asString();
                for(int k=0; k<root[i][j]["annorect"].size(); k++){
                    int tid = root[i][j]["annorect"][k]["track_id"].asInt();
                    if(!root[i][j]["annorect"][k].isMember("fake_rect")) continue;
                    float x1 = root[i][j]["annorect"][k]["fake_rect"][0].asFloat();
                    float y1 = root[i][j]["annorect"][k]["fake_rect"][1].asFloat();
                    float x2 = root[i][j]["annorect"][k]["fake_rect"][2].asFloat();
                    float y2 = root[i][j]["annorect"][k]["fake_rect"][3].asFloat();
                    tFrame.persons[tid] = cv::Rect(cv::Point(x1,y1), cv::Point(x2,y2));
                }
                tVideo.frames.emplace_back(tFrame);
            }
            videos.emplace_back(tVideo);
        }
    }

    mOPDataTransformer.reset(new OPDataTransformer<Dtype>(op_transform_param_));

    const int batch_size = this->layer_param_.data_param().batch_size();
    const int num_people_image = this->layer_param_.op_transform_param().num_people_image();

    // Multi Image shape (Data layer is ([frame*batch * 3 * 368 * 38])) - Set Data size
    const int width = this->layer_param_.op_transform_param().crop_size_x();
    const int height = this->layer_param_.op_transform_param().crop_size_y();
    std::vector<int> topShape{batch_size * triplet_size, 3, height, width};
    top[0]->Reshape(topShape);

    // Set output and prefetch size
    this->transformed_data_.Reshape(topShape[0], topShape[1], topShape[2], topShape[3]);
    for (int i = 0; i < this->prefetch_.size(); ++i)
        this->prefetch_[i]->data_.Reshape(topShape);
    LOG(INFO) << "Image shape: " << topShape[0] << ", " << topShape[1] << ", " << topShape[2] << ", " << topShape[3];

    // Labels
    if (this->output_labels_)
    {
        std::vector<int> labelShape{batch_size * triplet_size * num_people_image, 5};
        top[1]->Reshape(labelShape);
        for (int i = 0; i < this->prefetch_.size(); ++i)
            this->prefetch_[i]->label_.Reshape(labelShape);
        for (int i = 0; i < this->prefetch_.size(); ++i)
            for (int j = 0; j < Batch<float>::extra_labels_count; ++j)
                this->prefetch_[i]->extra_labels_[j].Reshape(labelShape);
        this->transformed_label_.Reshape(labelShape);

        // ID
        if(id_available){
            top[2]->Reshape(std::vector<int>{labelShape[0],1});
            for (int i = 0; i < this->prefetch_.size(); ++i){
                this->prefetch_[i]->extra_labels_[0].Reshape(std::vector<int>{labelShape[0],1});
            }
            LOG(INFO) << "ID shape: " << top[2]->shape()[0] << ", " << top[2]->shape()[1] << ", ";
        }

        LOG(INFO) << "Label shape: " << labelShape[0] << ", " << labelShape[1] << ", ";
    }
    else
        throw std::runtime_error{"output_labels_ must be set to true" + getLine(__LINE__, __FUNCTION__, __FILE__)};

    cout << "\t\t****Data Layer successfully initialized****" << endl;
}

template <typename Dtype>
bool OPTripletLayer<Dtype>::Skip()
{
    int size = Caffe::solver_count();
    int rank = Caffe::solver_rank();
    bool keep = (offset_ % size) == rank ||
                  // In test mode, only rank 0 runs, so avoid skipping
                  this->layer_param_.phase() == TEST;
    return !keep;
}

template<typename Dtype>
void OPTripletLayer<Dtype>::Next()
{
    cursor_->Next();
    if (!cursor_->valid())
    {
        LOG_IF(INFO, Caffe::root_solver())
                << "Restarting data prefetching from start.";
        cursor_->SeekToFirst();
    }
    offset_++;
}

// OpenPose: added
template <typename Dtype>
bool OPTripletLayer<Dtype>::SkipSecond()
{
    int size = Caffe::solver_count();
    int rank = Caffe::solver_rank();
    bool keep = (offsetSecond % size) == rank ||
                  // In test mode, only rank 0 runs, so avoid skipping
                  this->layer_param_.phase() == TEST;
    return !keep;
}

template<typename Dtype>
void OPTripletLayer<Dtype>::NextBackground()
{
    if (backgroundDb)
    {
        cursorBackground->Next();
        if (!cursorBackground->valid())
        {
            LOG_IF(INFO, Caffe::root_solver())
                    << "Restarting negatives data prefetching from start.";
            cursorBackground->SeekToFirst();
        }
    }
}

template<typename Dtype>
void OPTripletLayer<Dtype>::NextSecond()
{
    cursorSecond->Next();
    if (!cursorSecond->valid())
    {
        LOG_IF(INFO, Caffe::root_solver())
                << "Restarting second data prefetching from start.";
        cursorSecond->SeekToFirst();
    }
    offsetSecond++;
}
// OpenPose: added ended

float intersectionPercentage(cv::Rect a, cv::Rect b, bool a_select = false){
    float dx = min(a.br().x, b.br().x) - max(a.tl().x, b.tl().x);
    float dy = min(a.br().y, b.br().y) - max(a.tl().y, b.tl().y);
    float intersect_area = 0;
    if (dx >= 0 && dy >= 0) intersect_area = dx*dy;
    if(a_select) return intersect_area/a.area();
    else return max(intersect_area/a.area(), intersect_area/b.area());
}

std::pair<cv::Mat, cv::Size> rotateBoundSize(cv::Size currSize, float angle){
    int h = currSize.height;
    int w = currSize.width;
    int cx = w/2;
    int cy = h/2;

    cv::Mat M = cv::getRotationMatrix2D(cv::Point(cx,cy), -angle, 1.0);
    float cos = M.at<double>(0,0);
    float sin;
    if(angle < 0)
        sin = -M.at<double>(1,0);
    else
        sin = M.at<double>(1,0);
    int nW = int((h * sin) + (w * cos));
    int nH = int((h * cos) + (w * sin));

    M.at<double>(0,2) += (nW / 2) - cx;
    M.at<double>(1,2) += (nH / 2) - cy;

    return std::pair<cv::Mat, cv::Size>(M, cv::Size(nW, nH));
}

cv::Mat rotateBound(const cv::Mat& image, float angle){
    //angle = 10;

    std::pair<cv::Mat, cv::Size> data = rotateBoundSize(image.size(), angle);

    cv::Mat finalImage;
    cv::warpAffine(image, finalImage, data.first, data.second, cv::INTER_CUBIC, // CUBIC to consider rotations
                   cv::BORDER_CONSTANT, cv::Scalar{0,0,0});
    return finalImage;
}

void generateImage(const cv::Mat& backgroundImage, const std::vector<cv::Mat>& personImages, cv::Mat& finalImage, std::vector<cv::Rect>& rectangles){
/*
 * ? A mechanism to crop image, flip, scale rotate
 *
 * I take the images I have, 1st image, randomly sample some start index and scale
 * 2nd one do the same, keep sampling until no overlap
 * 3rd one do the same for both
 */
    fag:
    float size_scale = 0.33;
    float intersect_ratio = 0.2;
    float image_size_ratio = 1.1;
    float rotate_angle = 10;

    std::vector<cv::Rect> hold_rectangles;
    rectangles.clear();
    finalImage = backgroundImage.clone();
    for(int i=0; i<personImages.size(); i++){
        const cv::Mat& personImage = personImages[i];

        // Do the crop or rotation here!!
        // Warning, this is set to maxdim but set against width
        int maxDim = max(personImage.size().width, personImage.size().height);


        int counter = 0;
        int x, y, w, h;
        float rot;
        float curr_size_scale = size_scale;
        bool brokeLoop = false;
        while(1){
            // NEED A BETTER WAY TO HANDLE SCALE
            if(counter > 1000){
                //std::cout << "warning: reset try again" << std::endl;
                size_scale *= 0.9;
                rectangles.clear();
                hold_rectangles.clear();
                finalImage = backgroundImage.clone();
                i = -1;
                brokeLoop = true;
                break;
            }

            counter++;

            // size needs to be fixed
            w = getRand(maxDim*curr_size_scale, maxDim);
            h = w*((float)personImage.size().height/(float)personImage.size().width);
            x = getRand(0, fabs(finalImage.size().width - w));
            y = getRand(0, fabs(finalImage.size().height - h));
            cv::Rect hold_rect(x,y,w,h);

            // Rot
            rot = getRand(-rotate_angle,rotate_angle);
            cv::Size newPossibleSize;
            if(rot == 0)
                newPossibleSize = cv::Size(w,h);
            else
                newPossibleSize = rotateBoundSize(cv::Size(w,h), rot).second;
            x += (newPossibleSize.width - w) / 2;
            y += (newPossibleSize.height - h) / 2;
            w = newPossibleSize.width;
            h = newPossibleSize.height;

            if(w >= finalImage.size().width/image_size_ratio || h >= finalImage.size().height/image_size_ratio) continue;
            if(x < 0 || x >= finalImage.size().width || y < 0 || y >= finalImage.size().height) continue;
            if(x+w < 0 || x+w >= finalImage.size().width || y+h < 0 || y+h >= finalImage.size().height) continue;
            cv::Rect currentRect = cv::Rect(x,y,w,h);

            bool intersectFail = false;
            for(cv::Rect& otherRect : rectangles){
                if(intersectionPercentage(currentRect, otherRect) > intersect_ratio){
                    intersectFail = true;
                    break;
                }
            }
            if(intersectFail) continue;

            rectangles.emplace_back(currentRect);
            hold_rectangles.emplace_back(hold_rect);
            break;
        }

        if(brokeLoop){
            continue;
        }
        cv::Mat newPersonImage;
        cv::Rect rect = rectangles.back();
        cv::Rect hold_rect = hold_rectangles.back();
        cv::resize(personImage, newPersonImage,cv::Size(hold_rect.width, hold_rect.height));

        //cv::rectangle(finalImage, rect, cv::Scalar(255,0,0));

        cv::Mat mask = cv::Mat(newPersonImage.size(), CV_8UC3,cv::Scalar(255,255,255));
        mask = rotateBound(mask, rot);
        newPersonImage = rotateBound(newPersonImage, rot);

        //cv::Mat mask = newPersonImage.clone();
        newPersonImage.copyTo(finalImage(rect), mask);
    }

    //cv::imshow("win", finalImage);
    //cv::waitKey(1000);
}

template<typename Dtype>
void matToCaffeInt(Dtype* caffeImg, const cv::Mat& imgAug){
    const int imageAugmentedArea = imgAug.rows * imgAug.cols;
    auto* uCharPtrCvMat = (unsigned char*)(imgAug.data);
    //caffeImg = new Dtype[imgAug.channels()*imgAug.size().width*imgAug.size().height];
    for (auto y = 0; y < imgAug.rows; y++)
    {
        const auto yOffset = y*imgAug.cols;
        for (auto x = 0; x < imgAug.cols; x++)
        {
            const auto xyOffset = yOffset + x;
            // const cv::Vec3b& bgr = imageAugmented.at<cv::Vec3b>(y, x);
            auto* bgr = &uCharPtrCvMat[3*xyOffset];
            caffeImg[xyOffset] = (bgr[0] - 128) / 256.0;
            caffeImg[xyOffset + imageAugmentedArea] = (bgr[1] - 128) / 256.0;
            caffeImg[xyOffset + 2*imageAugmentedArea] = (bgr[2] - 128) / 256.0;
        }
    }
}

template<typename Dtype>
void caffeToMatInt(cv::Mat& img, const Dtype* caffeImg, cv::Size imageSize){
    // Need a function to convert back
    img = cv::Mat(imageSize, CV_8UC3);
    const int imageAugmentedArea = img.rows * img.cols;
    auto* imgPtr = (unsigned char*)(img.data);
    for (auto y = 0; y < img.rows; y++)
    {
        const auto yOffset = y*img.cols;
        for (auto x = 0; x < img.cols; x++)
        {
            const auto xyOffset = yOffset + x;
            auto* bgr = &imgPtr[3*xyOffset];
            bgr[0] = (caffeImg[xyOffset]*256.) + 128;
            bgr[1] = (caffeImg[xyOffset + imageAugmentedArea]*256.) + 128;
            bgr[2] = (caffeImg[xyOffset + 2*imageAugmentedArea]*256.) + 128;
        }
    }
}

// This function is called on prefetch thread
template<typename Dtype>
void OPTripletLayer<Dtype>::load_batch(Batch<Dtype>* batch)
{
//    CPUTimer batch_timer;
//    batch_timer.Start();
//    double read_time = 0;
//    double trans_time = 0;
//    CPUTimer timer;
//    CHECK(batch->data_.count());
//    CHECK(this->transformed_data_.count());
    const int batch_size = this->layer_param_.data_param().batch_size();
    const int total_images = batch_size * triplet_size;
    const int num_people_image = this->layer_param_.op_transform_param().num_people_image();

    // Get Label pointer [Label shape: 20, 132, 46, 46]
    auto* topLabel = batch->label_.mutable_cpu_data();
    for(int i=0; i<Batch<float>::extra_labels_count; i++)
        batch->extra_labels_[i].mutable_cpu_data();

    auto* topData = batch->data_.mutable_cpu_data();
    auto* labelData = batch->label_.mutable_cpu_data();

    Dtype* idData = nullptr;
    if(id_available){
        idData = batch->extra_labels_[0].mutable_cpu_data();
    }

    //std::cout << batch->data_.shape_string() << std::endl; // 9, 3, 368, 368
    //std::cout << batch->label_.shape_string() << std::endl; // 27, 5

    /*
     * 0. Store Path of Train Folder
     *      1. Save all the background paths into some dict[pid] = array(cv::Mat)
     * 1. Load 9 Backgrounds into CVMat with the LMDB
     * 2. Load 3(Batchsize * NumPeople) completely unique people / ids
     *      For each unique person, load one positive and load one negative (Comes to 9*3 = 27)
     *      [1 2 3 4 5 6 7 8 9] Ref
     *      [1 2 3 4 5 6 7 8 9] +
     *      [1 2 3 4 5 6 7 8 9] -
     * 3. Loop through 3 then 3 background
     *      For each set, spread 3 people equally on every image, diff size/rotation etc.
     *      Store the bounding box
     * 4. Setup the table
     */

    // Load background images
    std::vector<cv::Mat> backgroundImages;
    Datum datumBackground;
    for (int item_id = 0; item_id < total_images; ++item_id) {
        if (backgroundDb)
        {
            NextBackground();
            datumBackground.ParseFromString(cursorBackground->value());
            backgroundImages.emplace_back(mOPDataTransformer->parseBackground(&datumBackground));
        }
    }

    // Load Unique People IDS
    for(int i=0; i<batch_size; i++){

        auto* batchLabelPtr = labelData + batch->label_.offset(i * num_people_image * triplet_size);
        Dtype* batchIDPtr = nullptr;
        if(idData != nullptr) batchIDPtr = idData + batch->extra_labels_[0].offset(i * num_people_image * triplet_size);

        const float dice = static_cast <float> (rand()) / static_cast <float> (RAND_MAX); //[0,1]
        const auto desiredDbIs1 = !secondary_prob || (dice <= (1-secondary_prob));
        std::map<int, std::vector<std::string>>* reidData_ref;
        if(desiredDbIs1)
            reidData_ref = &reidData;
        else
            reidData_ref = &reidData_secondary;

        // Not Video Mode
        if(desiredDbIs1 || (!desiredDbIs1 && !videos.size())){

            std::vector< std::pair<int, std::vector<std::string>>> positive_ids, negative_ids; // 3 each
            for(int j=0; j<num_people_image; j++){
                positive_ids.emplace_back(*select_randomly(reidData_ref->begin(), reidData_ref->end()));
                negative_ids.emplace_back(*select_randomly(reidData_ref->begin(), reidData_ref->end()));
            }

            std::vector<cv::Mat> vizImages;
            for(int j=0; j<triplet_size; j++){
                int image_id = i*triplet_size + j;
                cv::Mat backgroundImage = backgroundImages[image_id];
                std::vector<cv::Mat> personImages;
                std::vector<int> personIDs;

                // J=0 Is for Reference Image
                if(j==0){
                    for(auto& pos_id : positive_ids){
                        cv::Mat pos_id_image = cv::imread(pos_id.second[getRand(0, pos_id.second.size()-1)]);
                        personImages.emplace_back(pos_id_image);
                        personIDs.emplace_back(pos_id.first);
                    }
                }
                // J=1 Is for +
                else if(j==1){
                    for(auto& pos_id : positive_ids){
                        cv::Mat pos_id_image = cv::imread(pos_id.second[getRand(0, pos_id.second.size()-1)]);
                        personImages.emplace_back(pos_id_image);
                        personIDs.emplace_back(pos_id.first);
                    }
                }
                // J=2 Is for -
                else if(j==2){
                    for(auto& neg_id : negative_ids){
                        cv::Mat neg_id_image = cv::imread(neg_id.second[getRand(0, neg_id.second.size()-1)]);
                        personImages.emplace_back(neg_id_image);
                        personIDs.emplace_back(neg_id.first);
                    }
                }

                // Generate Image
                cv::Mat finalImage; std::vector<cv::Rect> rects;
                generateImage(backgroundImage, personImages, finalImage, rects);
                vizImages.emplace_back(finalImage);

                // Convert image to Caffe
                matToCaffeInt(topData + batch->data_.offset(image_id), finalImage);

                // Write rects
                for(int k=0; k<rects.size(); k++){
                    cv::Rect& rect = rects[k];
                    int id = personIDs[k];
                    (batchLabelPtr + batch->label_.offset(k * triplet_size + j))[0] = image_id;
                    (batchLabelPtr + batch->label_.offset(k * triplet_size + j))[1] = rect.x;
                    (batchLabelPtr + batch->label_.offset(k * triplet_size + j))[2] = rect.y;
                    (batchLabelPtr + batch->label_.offset(k * triplet_size + j))[3] = rect.x + rect.width;
                    (batchLabelPtr + batch->label_.offset(k * triplet_size + j))[4] = rect.y + rect.height;

                    if(batchIDPtr != nullptr){
                        (batchIDPtr + batch->extra_labels_[0].offset(k * triplet_size + j))[0] = id;
                    }
                }

    //            // Visualize
    //            for(int k=0; k<rects.size(); k++){
    //                int final_id = -1;
    //                if(batchIDPtr != nullptr){
    //                    auto* testPtr = (batchIDPtr + batch->extra_labels_[0].offset(k * triplet_size + j));
    //                    final_id = testPtr[0];
    //                }
    //                cv::Rect& rect = rects[k];
    //                cv::putText(finalImage, std::to_string(final_id) + " " + std::to_string(image_id) + " " + std::to_string(rect.x + rect.width) + " " + std::to_string(rect.y + rect.height),  rect.tl(), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 0), 2);
    //                cv::imwrite("visualize2/"+std::to_string(image_id)+".jpg", finalImage);
    //            }

            }

            if(debug_mode){
                cv::imshow("anchor", vizImages[0]);
                cv::imshow("pos", vizImages[1]);
                cv::imshow("neg", vizImages[2]);
                cv::waitKey(2000);
            }
        }
        // Video Mode
        else{

            /* 1. We randomly select two videos
             * 2. We select 3 random frames (2 from same video, 1 from other video (or same video with more prob))
             * 3. We then select 3 random people from each frame (ensure a and + has same ID), if there are less than 3 we repeat it
             * 4. Visualize it
             */

            // SHOULD THE + VIDEO BE DONE INCREMENTALLY?
            static std::mutex mutex;
            static int global_counter = 0;
            int internal_counter = 0;
            mutex.lock();
            global_counter += 1;
            internal_counter = global_counter;
            mutex.unlock();
            //std::cout << "Internal: "  << internal_counter << std::endl;

            // OR IF WE RANDOMIZE MAKE SURE DONT SELECT SAME VID?
            bool intersect_log = false;

            // Pick Video wanted
            float diff_vid_prob = 0.5;
            const float dice = static_cast <float> (rand()) / static_cast <float> (RAND_MAX); //[0,1]
            const auto same_vid = !diff_vid_prob || (dice <= (1-diff_vid_prob));
            int pos_vid = internal_counter % (videos.size()-1);
            if(pos_vid == 0){
                float skip_percentage = (float)total_skips / (float)videos.size();
                std::cout << "SKIP Percentage: " << skip_percentage << std::endl;
                total_skips = 0;
            }
            //int pos_vid = getRand(0, videos.size()-1);
            int neg_vid = pos_vid;
            if(!same_vid) neg_vid = getRand(0, videos.size()-1);

            // HACK FOR NOW
            //pos_vid = 12;
            //neg_vid = 1;

            int anch_frame, pos_frame, neg_frame;
            std::vector<int> anch_people_chosen_ids; std::vector<cv::Rect> anch_people_chosen_rects;
            std::vector<int> pos_people_chosen_ids; std::vector<cv::Rect> pos_people_chosen_rects;
            std::vector<int> neg_people_chosen_ids; std::vector<cv::Rect> neg_people_chosen_rects;
            int ext_counter = 0;
            int skip_add = 0;
            while(1){
                ext_counter++;
                if(ext_counter > 800){
                    const float dice = static_cast <float> (rand()) / static_cast <float> (RAND_MAX); //[0,1]
                    const auto same_vid = !diff_vid_prob || (dice <= (1-diff_vid_prob));
                    int prev_pos_vid = pos_vid;
                    //pos_vid = ++curr_video % (videos.size()-1);
                    pos_vid = getRand(0, videos.size()-1);
                    neg_vid = pos_vid;
                    if(!same_vid) neg_vid = getRand(0, videos.size()-1);
                    ext_counter = 0;
                    skip_add = 1;
                    //std::cout << "Skipping Vid " << prev_pos_vid << std::endl;
                }

                // Reset var
                anch_people_chosen_ids.clear(); anch_people_chosen_rects.clear();
                pos_people_chosen_ids.clear(); pos_people_chosen_rects.clear();
                neg_people_chosen_ids.clear(); neg_people_chosen_rects.clear();

                // Pick 2 anchor and positive frames
                anch_frame = getRand(0, videos[pos_vid].frames.size()-1);
                pos_frame = getRand(0, videos[pos_vid].frames.size()-1);
                if(anch_frame == pos_frame) continue;
                neg_frame = getRand(0, videos[neg_vid].frames.size()-1);

                // If neg frame and pos frame are same, then the pos frame needs at least 2 people
                if(videos[pos_vid].frames[anch_frame].persons.size() < 2 ||
                   videos[pos_vid].frames[pos_frame].persons.size() < 2 ||
                   videos[neg_vid].frames[neg_frame].persons.size() < 1){
                    if(intersect_log) std::cout << "Video Frame has bad no of people" << std::endl;
                    continue;
                }

                // Iterate through each person
                bool pick_failed = false;

                int within_frame_counter = 0;
                for(int j=0; j<num_people_image; j++){
                    within_frame_counter++;

                    // Try to select a non-selected person always (if available)
                    // Pick a person ID from the anchor frame
                    std::pair<int, cv::Rect> person_data_anchor;
                    // If we have not tried to choose different ID enough
                    if(within_frame_counter < 200){
                        while(1){
                            person_data_anchor = *select_randomly(videos[pos_vid].frames[anch_frame].persons.begin(), videos[pos_vid].frames[anch_frame].persons.end());
                            if(anch_people_chosen_ids.size() == videos[pos_vid].frames[anch_frame].persons.size()) break;
                            if(vec_contains(anch_people_chosen_ids, person_data_anchor.first)) continue;
                            else break;
                        }
                    }
                    // If we have tried enough
                    else{
                        // We couldnt choose anyone
                        // Make sure at least 2 people in image
                        if(anch_people_chosen_ids.size() <= 1){
                            ext_counter += 100;
                            pick_failed = true;
                            break;
                        }
                        // We randomly select a succesful one again
                        int possible_id = *select_randomly(anch_people_chosen_ids.begin(), anch_people_chosen_ids.end());
                        person_data_anchor.first = possible_id;
                        person_data_anchor.second = videos[pos_vid].frames[anch_frame].persons[possible_id];
                    }
                    int person_data_anchor_id = person_data_anchor.first;
                    cv::Rect person_data_anchor_bbox = person_data_anchor.second;

                    // Possiblity of limiting the bounding box to face etc, then we need to store keypoints directly

                    // Check if anchor experiences significant overlap within the frame with other people
                    bool intersectBad = false;
                    for (auto& kv : videos[pos_vid].frames[anch_frame].persons) {
                        if(kv.first == person_data_anchor_id) continue;
                        if(intersectionPercentage(person_data_anchor_bbox, kv.second, true) > 0.35) intersectBad = true;
                    }
                    if(intersectBad){
                        if(intersect_log) std::cout << "VID: " << pos_vid << " Frame: " << anch_frame << " PID: " << person_data_anchor_id << " First Intersection Problem" << std::endl;
                        //pick_failed = true; break;
                        j-=1; continue;
                    }

                    // Check if the person is available in the pos frame
                    if(!videos[pos_vid].frames[pos_frame].persons.count(person_data_anchor_id)){
                        if(intersect_log) std::cout << "VID: " << pos_vid << " Frame: " << pos_frame << " PID: " << person_data_anchor_id << " Unable to find in other frame" << std::endl;
                        //pick_failed = true; break;
                        j-=1; continue;
                    }
                    cv::Rect person_data_pos_bbox = videos[pos_vid].frames[pos_frame].persons[person_data_anchor_id];

                    // Check if + experiences significant overlap within the frame with other people
                    intersectBad = false;
                    for (auto& kv : videos[pos_vid].frames[pos_frame].persons) {
                        if(kv.first == person_data_anchor_id) continue;
                        if(intersectionPercentage(person_data_pos_bbox, kv.second, true) > 0.35) intersectBad = true;
                    }
                    if(intersectBad){
                        if(intersect_log) std::cout << "VID: " << pos_vid << " Frame: " << pos_frame << " PID: " << person_data_anchor_id << " Second Intersection Problem" << std::endl;
                        //pick_failed = true; break;
                        j-=1; continue;
                    }

                    // Add some noise to the bounding boxes. contact or increase based on size
                    int anchor_increase_xmax = person_data_anchor_bbox.width/20;
                    int anchor_increase_ymax = person_data_anchor_bbox.height/20;
                    int pos_increase_xmax = person_data_pos_bbox.width/20;
                    int pos_increase_ymax = person_data_pos_bbox.height/20;
                    person_data_anchor_bbox.x += getRand(0, anchor_increase_xmax);
                    person_data_anchor_bbox.y += getRand(0, anchor_increase_ymax);
                    person_data_anchor_bbox.width -= getRand(0, anchor_increase_xmax);
                    person_data_anchor_bbox.height -= getRand(0, anchor_increase_ymax);
                    person_data_pos_bbox.x += getRand(0, pos_increase_xmax);
                    person_data_pos_bbox.y += getRand(0, pos_increase_ymax);
                    person_data_pos_bbox.width -= getRand(0, pos_increase_xmax);
                    person_data_pos_bbox.height -= getRand(0, pos_increase_ymax);

                    // Try select negative that is not in list if its the same neg vid
                    std::pair<int, cv::Rect> person_data_neg;
                    if(pos_vid == neg_vid){
                        int int_counter = 0;
                        while(1){
                            int_counter++;
                            if(int_counter > 100){
                                pick_failed = true;
                                break;
                            }

                            person_data_neg = *select_randomly(videos[neg_vid].frames[neg_frame].persons.begin(), videos[neg_vid].frames[neg_frame].persons.end());

                            // Dont choose a neg that is overlapped completely with another
                            intersectBad = false;
                            for (auto& kv : videos[neg_vid].frames[neg_frame].persons) {
                                if(kv.first == person_data_neg.first) continue;
                                if(intersectionPercentage(person_data_neg.second, kv.second, true) > 0.8) intersectBad = true;
                            }
                            if(intersectBad){
                                if(intersect_log) std::cout << "Neg selection intersect bad" << std::endl;
                                continue;
                            }

                            if(person_data_neg.first == person_data_anchor_id) continue;
                            else break;
                        }
                        if(pick_failed) break;
                    }else{
                        person_data_neg = *select_randomly(videos[neg_vid].frames[neg_frame].persons.begin(), videos[neg_vid].frames[neg_frame].persons.end());
                    }

                    // Add Anchor
                    anch_people_chosen_ids.emplace_back(person_data_anchor_id);
                    anch_people_chosen_rects.emplace_back(person_data_anchor_bbox);

                    // Add Pos
                    pos_people_chosen_ids.emplace_back(person_data_anchor_id);
                    pos_people_chosen_rects.emplace_back(person_data_pos_bbox);

                    // Add Neg
                    neg_people_chosen_ids.emplace_back(person_data_neg.first);
                    neg_people_chosen_rects.emplace_back(person_data_neg.second);
                }

                // Failed to find anything in this frame
                if(pick_failed){
                    if(intersect_log) std::cout << "REPICK**" << std::endl;
                    continue;
                }

                // Break
                if(anch_people_chosen_ids.size() == num_people_image) {
                    //std::cout << pos_vid << " " << neg_vid << " " << anch_frame << " " << pos_frame << " " << neg_frame << std::endl;
                    //std::cout << anch_people_chosen_ids[0] << " " << anch_people_chosen_ids[1] << " " << anch_people_chosen_ids[2] << std::endl;
                    //std::cout << pos_people_chosen_ids[0] << " " << pos_people_chosen_ids[1] << " " << pos_people_chosen_ids[2] << std::endl;
                    //std::cout << neg_people_chosen_ids[0] << " " << neg_people_chosen_ids[1] << " " << neg_people_chosen_ids[2] << std::endl;
                    break;
                }
            }

            // Skip add
            total_skips += skip_add;

            // OP Convert
            std::vector<cv::Mat> vizImages;
            for(int j=0; j<3; j++){
                cv::Mat img;
                std::vector<cv::Rect>* rects;
                int image_id = i*triplet_size + j;
                cv::Mat bg;
                if(backgroundImages.size()) bg = backgroundImages[image_id];

                // Anchor
                if(j==0){
                    img = cv::imread(videos[pos_vid].frames[anch_frame].imagePath);
                    img = mOPDataTransformer->opConvert(img, bg, anch_people_chosen_rects);
                    rects = &anch_people_chosen_rects;
                    cv::Mat vizImage = img.clone(); cv::rectangle(vizImage, anch_people_chosen_rects[0], cv::Scalar(255,0,0)); cv::rectangle(vizImage, anch_people_chosen_rects[1], cv::Scalar(0,255,0)); cv::rectangle(vizImage, anch_people_chosen_rects[2], cv::Scalar(00,0,255)); vizImages.emplace_back(vizImage);
                }
                // +
                if(j==1){
                    img = cv::imread(videos[pos_vid].frames[pos_frame].imagePath);
                    img = mOPDataTransformer->opConvert(img, bg, pos_people_chosen_rects);
                    rects = &pos_people_chosen_rects;
                    cv::Mat vizImage = img.clone(); cv::rectangle(vizImage, pos_people_chosen_rects[0], cv::Scalar(255,0,0)); cv::rectangle(vizImage, pos_people_chosen_rects[1], cv::Scalar(0,255,0)); cv::rectangle(vizImage, pos_people_chosen_rects[2], cv::Scalar(0,0,255)); vizImages.emplace_back(vizImage);
                }
                // -
                if(j==2){
                    img = cv::imread(videos[neg_vid].frames[neg_frame].imagePath);
                    img = mOPDataTransformer->opConvert(img, bg, neg_people_chosen_rects);
                    rects = &neg_people_chosen_rects;
                    cv::Mat vizImage = img.clone(); cv::putText(vizImage, std::to_string(neg_people_chosen_ids[0]), neg_people_chosen_rects[0].tl(), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 0), 2); cv::putText(vizImage, std::to_string(neg_people_chosen_ids[1]), cv::Point(neg_people_chosen_rects[1].tl().x, neg_people_chosen_rects[1].tl().y + 20), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 0), 2); cv::putText(vizImage, std::to_string(neg_people_chosen_ids[2]), cv::Point(neg_people_chosen_rects[2].tl().x, neg_people_chosen_rects[2].tl().y + 40), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 0), 2);
                    cv::rectangle(vizImage, neg_people_chosen_rects[0], cv::Scalar(255,0,0)); cv::rectangle(vizImage, neg_people_chosen_rects[1], cv::Scalar(0,255,0)); cv::rectangle(vizImage, neg_people_chosen_rects[2], cv::Scalar(0,0,255)); vizImages.emplace_back(vizImage);
                }

                // Convert image to Caffe

                matToCaffeInt(topData + batch->data_.offset(image_id), img);

                // Write rects
                for(int k=0; k<rects->size(); k++){
                    cv::Rect& rect = rects->at(k);
                    (batchLabelPtr + batch->label_.offset(k * triplet_size + j))[0] = image_id;
                    (batchLabelPtr + batch->label_.offset(k * triplet_size + j))[1] = rect.x;
                    (batchLabelPtr + batch->label_.offset(k * triplet_size + j))[2] = rect.y;
                    (batchLabelPtr + batch->label_.offset(k * triplet_size + j))[3] = rect.x + rect.width;
                    (batchLabelPtr + batch->label_.offset(k * triplet_size + j))[4] = rect.y + rect.height;
                }

//                // Visualize
//                cv::Mat cImg = img.clone();
//                for(int k=0; k<rects->size(); k++){
//                    cv::Rect& rect = rects->at(k);
//                    cv::putText(cImg, std::to_string(k) + " " + std::to_string(image_id) + " " + std::to_string(rect.x + rect.width) + " " + std::to_string(rect.y + rect.height),  rect.tl(), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 0), 2);
//                    cv::rectangle(cImg, rect, cv::Scalar((k==0)*255, (k==1)*255, (k==2)*255));
//                }
//                cv::imwrite("visualize2/"+std::to_string(image_id)+".jpg", cImg);

            }

            if(debug_mode){
                cv::imshow("anchor", vizImages[0]);
                cv::imshow("pos", vizImages[1]);
                cv::imshow("neg", vizImages[2]);
                cv::waitKey(2000);
            }


        }

    }

    //std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // Video Incremental Implement

    // Put BG Image on black areas

    //exit(-1);
}

INSTANTIATE_CLASS(OPTripletLayer);
REGISTER_LAYER_CLASS(OPTriplet);

}  // namespace caffe
