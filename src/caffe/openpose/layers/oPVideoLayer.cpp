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
#include "caffe/openpose/layers/oPVideoLayer.hpp"
// OpenPose: added end

#include <iostream>
using namespace std;

namespace caffe {

template <typename Dtype>
OPVideoLayer<Dtype>::OPVideoLayer(const LayerParameter& param) :
    BasePrefetchingDataLayer<Dtype>(param),
    offset_(),
    offsetSecond(), // OpenPose: added
    op_transform_param_(param.op_transform_param()) // OpenPose: added
{
    db_.reset(db::GetDB(param.data_param().backend()));
    db_->Open(param.data_param().source(), db::READ);
    cursor_.reset(db_->NewCursor());
    // OpenPose: added
    mOnes = 0;
    mTwos = 0;
    mThrees = 0;
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
    // Set up tertiary DB
    if (!param.op_transform_param().source_tertiary().empty())
    {
        thirdDb = true;
        thirdProbability = param.op_transform_param().prob_tertiary();
        CHECK_GE(thirdProbability, 0.f);
        CHECK_LE(thirdProbability, 1.f);
        dbThird.reset(db::GetDB(DataParameter_DB::DataParameter_DB_LMDB));
        dbThird->Open(param.op_transform_param().source_tertiary(), db::READ);
        cursorThird.reset(dbThird->NewCursor());
    }
    else
    {
        thirdDb = false;
        thirdProbability = 0.f;
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
    // A DB
    if (!param.op_transform_param().source_a().empty())
    {
        ADb = true;
        AProbability = param.op_transform_param().prob_a();
        CHECK_GE(AProbability, 0.f);
        CHECK_LE(AProbability, 1.f);
        dbA.reset(db::GetDB(DataParameter_DB::DataParameter_DB_LMDB));
        dbA->Open(param.op_transform_param().source_a(), db::READ);
        cursorA.reset(dbA->NewCursor());
    }
    else
    {
        ADb = false;
        AProbability = 0.f;
    }
    // B DB
    if (!param.op_transform_param().source_b().empty())
    {
        BDb = true;
        BProbability = param.op_transform_param().prob_b();
        CHECK_GE(BProbability, 0.f);
        CHECK_LE(BProbability, 1.f);
        dbB.reset(db::GetDB(DataParameter_DB::DataParameter_DB_LMDB));
        dbB->Open(param.op_transform_param().source_b(), db::READ);
        cursorB.reset(dbB->NewCursor());
    }
    else
    {
        BDb = false;
        BProbability = 0.f;
    }
    // Timer
    mDuration = 0;
    mCounter = 0;
    // OpenPose: added end
}

template <typename Dtype>
OPVideoLayer<Dtype>::~OPVideoLayer()
{
    this->StopInternalThread();
}


template <typename Dtype>
void OPVideoLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top)
{
    // If Staf
    std::vector<int> staf_ids;
    if(op_transform_param_.staf()){
        const std::string staf_ids_string = op_transform_param_.staf_ids();
        std::vector<std::string> strs;
        boost::split(strs,staf_ids_string,boost::is_any_of(" "));
        for(int i=0; i<strs.size(); i++){
            staf_ids.emplace_back(std::stoi(strs[i]));
        }
    }

    frame_size = this->layer_param_.op_transform_param().frame_size();

    const int batch_size = this->layer_param_.data_param().batch_size();
    // Read a data point, and use it to initialize the top blob.
    Datum datum;
    datum.ParseFromString(cursor_->value());

    // OpenPose Module
    mOPDataTransformer.reset(new OPDataTransformer<Dtype>(op_transform_param_, this->phase_, op_transform_param_.model(), op_transform_param_.tpaf(), op_transform_param_.staf(), staf_ids));
    if (secondDb)
        mOPDataTransformerSecondary.reset(new OPDataTransformer<Dtype>(op_transform_param_, this->phase_, op_transform_param_.model_secondary(), op_transform_param_.tpaf(), op_transform_param_.staf(), staf_ids));
    if (thirdDb)
        mOPDataTransformerTertiary.reset(new OPDataTransformer<Dtype>(op_transform_param_, this->phase_, op_transform_param_.model_tertiary(), op_transform_param_.tpaf(), op_transform_param_.staf(), staf_ids));
    if (ADb)
        mOPDataTransformerA.reset(new OPDataTransformer<Dtype>(op_transform_param_, this->phase_, op_transform_param_.model_a(), op_transform_param_.tpaf(), op_transform_param_.staf(), staf_ids));
    if (BDb)
        mOPDataTransformerB.reset(new OPDataTransformer<Dtype>(op_transform_param_, this->phase_, op_transform_param_.model_b(), op_transform_param_.tpaf(), op_transform_param_.staf(), staf_ids));

    // Multi Image shape (Data layer is ([frame*batch * 3 * 368 * 38])) - Set Data size
    const int width = this->phase_ != TRAIN ? datum.width() : this->layer_param_.op_transform_param().crop_size_x();
    const int height = this->phase_ != TRAIN ? datum.height() : this->layer_param_.op_transform_param().crop_size_y();
    std::vector<int> topShape{batch_size * frame_size, 3, height, width};
    top[0]->Reshape(topShape);

    // Set output and prefetch size
    this->transformed_data_.Reshape(topShape[0], topShape[1], topShape[2], topShape[3]);
    for (int i = 0; i < this->prefetch_.size(); ++i)
        this->prefetch_[i]->data_.Reshape(topShape);
    LOG(INFO) << "Video shape: " << topShape[0] << ", " << topShape[1] << ", " << topShape[2] << ", " << topShape[3];

    // Labels
    if (this->output_labels_)
    {
        const int stride = this->layer_param_.op_transform_param().stride();
        const int numberChannels = this->mOPDataTransformer->getNumberChannels();
        std::vector<int> labelShape{batch_size * frame_size, numberChannels, height/stride, width/stride};
        top[1]->Reshape(labelShape);
        for (int i = 0; i < this->prefetch_.size(); ++i)
            this->prefetch_[i]->label_.Reshape(labelShape);
        for (int i = 0; i < this->prefetch_.size(); ++i)
            for (int j = 0; j < Batch<float>::extra_labels_count; ++j)
                this->prefetch_[i]->extra_labels_[j].Reshape(labelShape);
        this->transformed_label_.Reshape(labelShape[0], labelShape[1], labelShape[2], labelShape[3]);
        LOG(INFO) << "Label shape: " << labelShape[0] << ", " << labelShape[1] << ", " << labelShape[2] << ", " << labelShape[3];
    }
    else
        throw std::runtime_error{"output_labels_ must be set to true" + getLine(__LINE__, __FUNCTION__, __FILE__)};

    cout << "\t\t****Data Layer successfully initialized****" << endl;
}

template <typename Dtype>
bool OPVideoLayer<Dtype>::Skip()
{
    int size = Caffe::solver_count();
    int rank = Caffe::solver_rank();
    bool keep = (offset_ % size) == rank ||
                  // In test mode, only rank 0 runs, so avoid skipping
                  this->layer_param_.phase() == TEST;
    return !keep;
}

template<typename Dtype>
void OPVideoLayer<Dtype>::Next()
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
bool OPVideoLayer<Dtype>::SkipSecond()
{
    int size = Caffe::solver_count();
    int rank = Caffe::solver_rank();
    bool keep = (offsetSecond % size) == rank ||
                  // In test mode, only rank 0 runs, so avoid skipping
                  this->layer_param_.phase() == TEST;
    return !keep;
}

// OpenPose: added
template <typename Dtype>
bool OPVideoLayer<Dtype>::SkipThird()
{
    int size = Caffe::solver_count();
    int rank = Caffe::solver_rank();
    bool keep = (offsetThird % size) == rank ||
                  // In test mode, only rank 0 runs, so avoid skipping
                  this->layer_param_.phase() == TEST;
    return !keep;
}

// OpenPose: added
template <typename Dtype>
bool OPVideoLayer<Dtype>::SkipA()
{
    int size = Caffe::solver_count();
    int rank = Caffe::solver_rank();
    bool keep = (offsetA % size) == rank ||
                  // In test mode, only rank 0 runs, so avoid skipping
                  this->layer_param_.phase() == TEST;
    return !keep;
}

// OpenPose: added
template <typename Dtype>
bool OPVideoLayer<Dtype>::SkipB()
{
    int size = Caffe::solver_count();
    int rank = Caffe::solver_rank();
    bool keep = (offsetB % size) == rank ||
                  // In test mode, only rank 0 runs, so avoid skipping
                  this->layer_param_.phase() == TEST;
    return !keep;
}

template<typename Dtype>
void OPVideoLayer<Dtype>::NextBackground()
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
void OPVideoLayer<Dtype>::NextSecond()
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

template<typename Dtype>
void OPVideoLayer<Dtype>::NextThird()
{
    cursorThird->Next();
    if (!cursorThird->valid())
    {
        LOG_IF(INFO, Caffe::root_solver())
                << "Restarting third data prefetching from start.";
        cursorThird->SeekToFirst();
    }
    offsetThird++;
}

template<typename Dtype>
void OPVideoLayer<Dtype>::NextA()
{
    cursorA->Next();
    if (!cursorA->valid())
    {
        LOG_IF(INFO, Caffe::root_solver())
                << "Restarting A data prefetching from start.";
        cursorA->SeekToFirst();
    }
    offsetA++;
}

template<typename Dtype>
void OPVideoLayer<Dtype>::NextB()
{
    cursorB->Next();
    if (!cursorB->valid())
    {
        LOG_IF(INFO, Caffe::root_solver())
                << "Restarting B data prefetching from start.";
        cursorB->SeekToFirst();
    }
    offsetB++;
}
// OpenPose: added ended

template<typename Dtype>
void OPVideoLayer<Dtype>::sample_dbs(bool &desiredDbIs1, bool &desiredDbIs2, bool &desiredDbIs3){
    const float dice = static_cast <float> (rand()) / static_cast <float> (RAND_MAX); //[0,1]
    desiredDbIs1 = true, desiredDbIs2 = false, desiredDbIs3 = false;
    if(!thirdDb){
        float firstProbability = (1-(secondProbability));
        if(dice <= firstProbability){
            desiredDbIs1 = true; desiredDbIs2 = false; desiredDbIs3 = false;
        }else{
            desiredDbIs1 = false; desiredDbIs2 = true; desiredDbIs3 = false;
        }
    }else{
        float firstProbability = (1-(secondProbability+thirdProbability));
        if(dice <= firstProbability){
            desiredDbIs1 = true; desiredDbIs2 = false; desiredDbIs3 = false;
        }else if(dice <= (firstProbability + secondProbability)){
            desiredDbIs1 = false; desiredDbIs2 = true; desiredDbIs3 = false;
        }else if(dice <= (firstProbability + secondProbability + thirdProbability)){
            desiredDbIs1 = false; desiredDbIs2 = false; desiredDbIs3 = true;
        }
    }
}

template<typename Dtype>
void OPVideoLayer<Dtype>::sample_ab(bool &desiredDbA, bool &desiredDbB){
    const float dice = static_cast <float> (rand()) / static_cast <float> (RAND_MAX); //[0,1]
    desiredDbA = true, desiredDbB = false;
    if(dice <= AProbability){
        desiredDbA = true; desiredDbB = false;
    }else{
        desiredDbA = false; desiredDbB = true;
    }
}

// This function is called on prefetch thread
template<typename Dtype>
void OPVideoLayer<Dtype>::load_batch(Batch<Dtype>* batch)
{
    CPUTimer batch_timer;
    batch_timer.Start();
    double read_time = 0;
    double trans_time = 0;
    CPUTimer timer;
    CHECK(batch->data_.count());
    CHECK(this->transformed_data_.count());
    const int batch_size = this->layer_param_.data_param().batch_size();

    // Get Label pointer [Label shape: 20, 132, 46, 46]
    auto* topLabel = batch->label_.mutable_cpu_data();
    for(int i=0; i<Batch<float>::extra_labels_count; i++)
        batch->extra_labels_[i].mutable_cpu_data();

    // OpenPose: added
    float video_or_image = 0.0;
    if(AProbability || BProbability) video_or_image = static_cast <float> (rand()) / static_cast <float> (RAND_MAX); //[0,1]
    //video_or_image = 1.0;

    //Change code so that its in image mode doesnt mix?

    bool desiredDbIs1 = false, desiredDbIs2 = false, desiredDbIs3 = false;
    bool desiredDbIsA = false, desiredDbIsB = false;
    //sample_dbs(desiredDbIs1, desiredDbIs2, desiredDbIs3);

    // Sample Outside
    if(video_or_image >= this->layer_param_.op_transform_param().video_or_image()){
        sample_dbs(desiredDbIs1, desiredDbIs2, desiredDbIs3);
    }else{
        sample_ab(desiredDbIsA, desiredDbIsB);
    }

    // Sample lmdb for video?
    Datum datum;
    Datum datumBackground;
    for (int item_id = 0; item_id < batch_size; ++item_id) {
        //const float dice = static_cast <float> (rand()) / static_cast <float> (RAND_MAX); //[0,1]
        //const auto desiredDbIs1 = !secondDb || (dice <= (1-secondProbability));
//        if(video_or_image >= this->layer_param_.op_transform_param().video_or_image()){
//            sample_dbs(desiredDbIs1, desiredDbIs2, desiredDbIs3);
//        }else{
//            if(item_id == 0) sample_ab(desiredDbIsA, desiredDbIsB);
//        }

        // Read from desired DB - DB1, DB2 or BG
        timer.Start();
        auto oPDataTransformerPtr = this->mOPDataTransformer;
        if (desiredDbIs1)
        {
            mOnes++;
            while (Skip())
                Next();
            datum.ParseFromString(cursor_->value());
        }
        // 2nd DB
        else if (desiredDbIs2)
        {
            oPDataTransformerPtr = this->mOPDataTransformerSecondary;
            mTwos++;
            while (SkipSecond())
                NextSecond();
            datum.ParseFromString(cursorSecond->value());
        }
        // 3rd DB
        else if (desiredDbIs3)
        {
            oPDataTransformerPtr = this->mOPDataTransformerTertiary;
            mThrees++;
            while (SkipThird())
                NextThird();
            datum.ParseFromString(cursorThird->value());
        }
        // A DB
        else if (desiredDbIsA)
        {
            oPDataTransformerPtr = this->mOPDataTransformerA;
            mAs++;
            while (SkipA())
                NextA();
            datum.ParseFromString(cursorA->value());
        }
        // B DB
        else if (desiredDbIsB)
        {
            oPDataTransformerPtr = this->mOPDataTransformerB;
            mBs++;
            while (SkipB())
                NextB();
            datum.ParseFromString(cursorB->value());
        }
        if (backgroundDb)
        {
            NextBackground();
            datumBackground.ParseFromString(cursorBackground->value());
        }
        read_time += timer.MicroSeconds();

        // First item
        if (item_id == 0) {
            const int width = this->phase_ != TRAIN ? datum.width() : this->layer_param_.op_transform_param().crop_size_x();
            const int height = this->phase_ != TRAIN ? datum.height() : this->layer_param_.op_transform_param().crop_size_y();
            batch->data_.Reshape({batch_size * frame_size, 3, height, width});
        }

        // Read in data
        timer.Start();
        VSeq vs;
        const int offset = batch->data_.offset(item_id);
        auto* topData = batch->data_.mutable_cpu_data();
        this->transformed_data_.set_cpu_data(topData);
        // Label
        const int offsetLabel = batch->label_.offset(item_id);
        this->transformed_label_.set_cpu_data(topLabel);
        // Process image & label
        const auto begin = std::chrono::high_resolution_clock::now();
        if (backgroundDb){
            if(desiredDbIs1)
                oPDataTransformerPtr->TransformVideoJSON(item_id, frame_size, vs, &(this->transformed_data_),
                                                &(this->transformed_label_),
                                                datum, &datumBackground);
            else if (desiredDbIs2 || desiredDbIs3)
                oPDataTransformerPtr->TransformVideoSF(item_id, frame_size, vs, &(this->transformed_data_),
                                                &(this->transformed_label_),
                                                datum, &datumBackground);
            else if (desiredDbIsA || desiredDbIsB)
                oPDataTransformerPtr->TransformVideoSF(item_id, frame_size, vs, &(this->transformed_data_),
                                                &(this->transformed_label_),
                                                datum, &datumBackground, false);

        }else{
            throw std::runtime_error("Not implemented");
        }
        const auto end = std::chrono::high_resolution_clock::now();
        mDuration += std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count();

        // Advance to next data
        if (desiredDbIs1)
            Next();
        else if (desiredDbIs2)
            NextSecond();
        else if (desiredDbIs3)
            NextThird();
        else if (desiredDbIsA)
            NextA();
        else if (desiredDbIsB)
            NextB();
        trans_time += timer.MicroSeconds();
    }

    // Testing Optional
    //if(vCounter == 2){
    //auto oPDataTransformerPtr = this->mOPDataTransformer;
    //oPDataTransformerPtr->Test(frame_size, &(this->transformed_data_), &(this->transformed_label_));
    //}
    //boost::this_thread::sleep_for(boost::chrono::milliseconds(1000));
    //std::cout << "Loaded Data" << std::endl;

    // Timer (every 20 iterations x batch size)
    mCounter++;
    vCounter++;
    const auto repeatEveryXVisualizations = 2;
    if (mCounter == 20*repeatEveryXVisualizations)
    {
        std::cout << "Time: " << mDuration/repeatEveryXVisualizations * 1e-9 << "s\t"
                  << "Ratio: " << mOnes/float(mOnes+mTwos) << std::endl;
        mDuration = 0;
        mCounter = 0;
    }
    timer.Stop();
    batch_timer.Stop();
    DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
    DLOG(INFO) << "     Read time: " << read_time / 1000 << " ms.";
    DLOG(INFO) << "Transform time: " << trans_time / 1000 << " ms.";
}

INSTANTIATE_CLASS(OPVideoLayer);
REGISTER_LAYER_CLASS(OPVideo);

}  // namespace caffe
