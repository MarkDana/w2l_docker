/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <cereal/archives/json.hpp>
#include <cereal/types/unordered_map.hpp>
#include <flashlight/flashlight.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "common/Defines.h"
#include "common/Dictionary.h"
#include "common/Transforms.h"
#include "common/Utils.h"
#include "criterion/criterion.h"
#include "data/Featurize.h"
#include "module/module.h"
#include "runtime/runtime.h"

#include "data/W2lDataset.h"
#include "data/W2lNumberedFilesDataset.h"

using namespace w2l;


int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  std::string exec(argv[0]);
  std::vector<std::string> argvs;
  for (int i = 0; i < argc; i++) {
    argvs.emplace_back(argv[i]);
  }
  gflags::SetUsageMessage(
      "Usage: \n " + exec + " train [flags]\n or " + std::string() +
      " continue [directory] [flags]\n or " + std::string(argv[0]) +
      " fork [directory/model] [flags]");

  /* ===================== Parse Options ===================== */
  int runIdx = 1; // current #runs in this path
  std::string runPath; // current experiment path
  std::string reloadPath; // path to model to reload
  std::string runStatus = argv[1];
  int startEpoch = 0;

  std::shared_ptr<fl::Module> network;
  std::shared_ptr<SequenceCriterion> criterion;
  std::unordered_map<std::string, std::string> cfg;
  std::vector<fl::Variable> pretrained_params;

  if (argc <= 1) {
    LOG(FATAL) << gflags::ProgramUsage();
  }

  if (runStatus == "fork") {
    reloadPath = argv[2];
    /* ===================== Create Network ===================== */
    LOG(INFO) << "Network reading pre-trained model from " << reloadPath;
    W2lSerializer::load(reloadPath, cfg, network, criterion);
    pretrained_params = network->params();

    //pre-trained network architecture
    LOG(INFO) << "[Network] is " << network->prettyString();
    LOG(INFO) << "[Criterion] is " << criterion->prettyString();
    LOG(INFO) << "[Network] params size is " << network->params().size();
    LOG(INFO) << "[Network] number of params is " << numTotalParams(network);

    //pre-trained network flags
    auto flags = cfg.find(kGflags);
    if (flags == cfg.end()) {
      LOG(FATAL) << "Invalid config loaded from " << reloadPath;
    }

    LOG(INFO) << "Reading flags from config file " << reloadPath;
    gflags::ReadFlagsFromString(flags->second, gflags::GetArgv0(), true);

    if (argc > 3) {
      LOG(INFO) << "Parsing command line flags";
      LOG(INFO) << "Overriding flags should be mutable when using `fork`";
      gflags::ParseCommandLineFlags(&argc, &argv, false);
    }

    if (!FLAGS_flagsfile.empty()) {
      LOG(INFO) << "Reading flags from file" << FLAGS_flagsfile;
      gflags::ReadFromFlagsFile(FLAGS_flagsfile, argv[0], true);
    }
    runPath = newRunPath(FLAGS_rundir, FLAGS_runname, FLAGS_tag);
  } else {
    LOG(FATAL) << gflags::ProgramUsage();
  }

  af::setMemStepSize(FLAGS_memstepsize);
  af::setSeed(FLAGS_seed);
  af::setFFTPlanCacheSize(FLAGS_fftcachesize);

  maybeInitDistributedEnv(
      FLAGS_enable_distributed,
      FLAGS_world_rank,
      FLAGS_world_size,
      FLAGS_rndv_filepath);

  auto worldRank = fl::getWorldRank();
  auto worldSize = fl::getWorldSize();

  bool isMaster = (worldRank == 0);

  LOG_MASTER(INFO) << "Gflags after parsing \n" << serializeGflags("; ");

  LOG_MASTER(INFO) << "Experiment path: " << runPath;
  LOG_MASTER(INFO) << "Experiment runidx: " << runIdx;

  std::unordered_map<std::string, std::string> config = {
      {kProgramName, exec},
      {kCommandLine, join(" ", argvs)},
      {kGflags, serializeGflags()},
      // extra goodies
      {kUserName, getEnvVar("USER")},
      {kHostName, getEnvVar("HOSTNAME")},
      {kTimestamp, getCurrentDate() + ", " + getCurrentDate()},
      {kRunIdx, std::to_string(runIdx)},
      {kRunPath, runPath}};

  /* ===================== Create Dictionary & Lexicon ===================== */
  Dictionary dict = createTokenDict();
  int numClasses = dict.indexSize();
  LOG_MASTER(INFO) << "Number of classes (network) = " << numClasses;

  DictionaryMap dicts;
  dicts.insert({kTargetIdx, dict});

  LexiconMap lexicon;
  if (FLAGS_listdata) {
    lexicon = loadWords(FLAGS_lexicon, FLAGS_maxword);
  }

  /* =========== Create Network & Optimizers / Reload Snapshot ============ */
  // network, criterion have been loaded before
  std::shared_ptr<fl::FirstOrderOptimizer> netoptim;
  std::shared_ptr<fl::FirstOrderOptimizer> critoptim;
  if (runStatus == "train" || runStatus == "fork") {
    netoptim = initOptimizer(
        network, FLAGS_netoptim, FLAGS_lr, FLAGS_momentum, FLAGS_weightdecay);
    critoptim =
        initOptimizer(criterion, FLAGS_critoptim, FLAGS_lrcrit, 0.0, 0.0);
  }
  LOG_MASTER(INFO) << "[Network Optimizer] " << netoptim->prettyString();
  LOG_MASTER(INFO) << "[Criterion Optimizer] " << critoptim->prettyString();

  /* ===================== Meters ===================== */
  

  /* ===================== Logging ===================== */
  

  /* ===================== Create Dataset ===================== */
  auto trainds = createDataset(
      FLAGS_train, dicts, lexicon, FLAGS_batchsize, worldRank, worldSize);


  /* ===================== Hooks ===================== */

  double gradNorm = 1.0 / (FLAGS_batchsize * worldSize);

  auto train = [gradNorm,
                pretrained_params,
                &startEpoch](
                   std::shared_ptr<fl::Module> ntwrk,
                   std::shared_ptr<SequenceCriterion> crit,
                   std::shared_ptr<W2lDataset> trainset,
                   std::shared_ptr<fl::FirstOrderOptimizer> netopt,
                   std::shared_ptr<fl::FirstOrderOptimizer> critopt,
                   double initlr,
                   double initcritlr,
                   bool clampCrit,
                   int nepochs) {
    fl::distributeModuleGrads(ntwrk, gradNorm);
    fl::distributeModuleGrads(crit, gradNorm);

    // synchronize parameters across processes
    fl::allReduceParameters(ntwrk);
    fl::allReduceParameters(crit);


    int64_t curEpoch = startEpoch;
    int64_t sampleIdx = 0;
    while (curEpoch < nepochs) {
      double lrScale = std::pow(FLAGS_gamma, curEpoch / FLAGS_stepsize);
      netopt->setLr(lrScale * initlr);
      critopt->setLr(lrScale * initcritlr);

      ++curEpoch;
      ntwrk->train();
      crit->train();

      af::sync();
      
      LOG_MASTER(INFO) << "Epoch " << curEpoch << " started!";

      //the size of trainset is just 1.
      auto pre_sample = trainset->get(0); //make noises for one audio sample
      int numNoise = 10; //make 1000 noise sub-samples for the audio sample
      std::vector<float> Yloss(numNoise); //loss written into Yloss
      std::ofstream Yfile("/root/w2l/CTC/loss.txt", std::ios::out);
      std::ofstream Mmeanfile("/root/w2l/CTC/m_mean.txt", std::ios::out);
      std::ofstream Mvarfile("/root/w2l/CTC/m_var.txt", std::ios::out);
      std::ofstream Mlossfile("/root/w2l/CTC/m_loss.txt", std::ios::out);
      std::ofstream mylossfile("/root/w2l/CTC/myloss.txt", std::ios::out);
      std::ofstream myloss_grad_mean_file("/root/w2l/CTC/myloss_grad_mean.txt", std::ios::out);
      std::ofstream myloss_grad_var_file("/root/w2l/CTC/myloss_grad_var.txt", std::ios::out);
      std::ofstream mloss_grad_mean_file("/root/w2l/CTC/mloss_grad_mean.txt", std::ios::out);
      std::ofstream mloss_grad_var_file("/root/w2l/CTC/mloss_grad_var.txt", std::ios::out);

      
      
      //std::vector<float> firGradnorm(numNoise);
      //std::ofstream firGradnormFile("/root/w2l/aboutM/firGradnorm.txt", std::ios::out);
      //std::vector<float> secGradnorm(numNoise);
      //std::ofstream secGradnormFile("/root/w2l/aboutM/secGradnorm.txt", std::ios::out);
      //std::vector<float> totGradnorm(numNoise);
      //std::ofstream totGradnormFile("/root/w2l/aboutM/totGradnorm.txt", std::ios::out);

      af::dim4 noiseDims = pre_sample[kFftIdx].dims(); //2K x T x FLAGS_channels x batchSz
      auto m = af::constant(0.1, noiseDims);
      //auto m = af::constant(0.1,noiseDims);
      //auto m=fl::normal(noiseDims,0.002,0.1).array();
      // float mylr = 0.001;
      float mylr = 1.0;

      //the previous network's output f*
      fl::Variable preOutput; 
      //W2lSerializer::load("/root/w2l/rawEmission.bin", preOutput);

      //pre_sample[kInputIdx] dims: T x K(257) x 1 x 1
      LOG_MASTER(INFO) << "pre_sample[kInputIdx] dims: " << pre_sample[kInputIdx].dims();
      //pre_sample[kFftIdx] dims: 2K(514) x T x 1 x 1
      LOG_MASTER(INFO) << "pre_sample[kFftIdx] dims: " << pre_sample[kFftIdx].dims();
      const float fftmean = af::mean<float>(pre_sample[kFftIdx]);
      const float fftstdev = af::stdev<float>(pre_sample[kFftIdx]);
      //LOG_MASTER(INFO) << af::toString("pre_sample fft's 6 values :", pre_sample[kFftIdx](af::seq(6)));
      LOG_MASTER(INFO) << "fft mean is:" << af::mean<float>(pre_sample[kFftIdx]);//-0.12
      LOG_MASTER(INFO) << "fft stdev is:" << af::stdev<float>(pre_sample[kFftIdx]);//4268.81
      //LOG_MASTER(INFO) << "dft mean is:" << af::mean<float>(pre_sample[kInputIdx]);//2136.15
      //LOG_MASTER(INFO) << "dft stdev is:" << af::stdev<float>(pre_sample[kInputIdx]);//5646.45

      std::ofstream preinput("/root/w2l/CTC/preFft.txt");
      if(preinput.is_open())
      {
        preinput << af::toString("pre_fft values:",pre_sample[kFftIdx]);
        preinput.close();
      }
      //using network to generate preOutput 
      auto prefinalinput=pre_sample[kInputIdx];
      const float inputmean=af::mean<float>(pre_sample[kInputIdx]);
      const float inputstdev=af::stdev<float>(pre_sample[kInputIdx]);
      prefinalinput= (prefinalinput-inputmean)/inputstdev;
      fl::Variable pretruefinalinput(prefinalinput,false);
  
      ntwrk->eval();
      crit->eval();
      preOutput = ntwrk->forward({pretruefinalinput}).front();
      auto preOutput_arr=preOutput.array();
      af::sync();

      af::dim4 outputDims = preOutput_arr.dims();
      int tokendim=outputDims[0];
      std::vector<int> axes{1};
      fl::Variable tmpOutput = fl::sqrt(fl::var(preOutput,axes));
      tmpOutput = fl::tileAs(tmpOutput, preOutput);
      fl::Variable addpreOutput = preOutput/tmpOutput;
	//for (size_t i = 0; i < tokendim; i=i+1)
      //{
        //  auto framestdev=af::stdev<float>(preOutput_arr(i,af::span,af::span,af::span));
          //preOutput_arr(i,af::span,af::span,af::span)=preOutput_arr(i,af::span,af::span,af::span)/framestdev;
          
      //}


      std::ofstream preOutFile("/root/w2l/CTC/preOutput.txt");
      if(preOutFile.is_open())
      {
  preOutFile << af::toString("preOutput is:", preOutput_arr);
  preOutFile.close();
      }

      // af::array zerowgt = af::identity(31,31);
      // zerowgt(0, 0) = 0;
      // zerowgt(1, 1) = 0;
  
      //   zerowgt(28, 28) = 0;
      //   zerowgt(29, 29) = 0;
      //   zerowgt(30, 30) = 0;

      // fl::Variable zeroweight(zerowgt, true);
	// auto addpreOutput = fl::Variable(af::matmul(zerowgt, preOutput.array()), false);
        //fl::Variable addpreOutput(preOutput_arr,true);
        //auto softmax_preOutput = fl::softmax(addpreOutput,1);
	// ignore 5 dimensions, softmax rest dimensions
	//auto tmpout = softmax_preOutput(af::seq(2, 27), af::span, af::span, af::span);
	auto tmpout = addpreOutput(af::seq(2, 27), af::span, af::span, af::span);
  auto softmax_add_preOutput = fl::softmax(tmpout, 0);
	//softmax_preOutput(af::seq(2,27),af::span,af::span,af::span)=softmax_tmpOut;
  // addpreOutput(af::seq(2,27),af::span,af::span,af::span)=softmax_tmpOut;
//	auto softmax_tmpOut = fl::softmax(tmpout,1);
//	auto softmax_preOut = fl::tileAs(softmax_tmpOut, softmax_preOutput.array().dims());
	// auto softmax_add_preOutput = fl::matmul(zeroweight, softmax_preOutput);
  // auto softmax_add_preOutput = fl::matmul(zeroweight, addpreOutput);



      std::ofstream preOutFile_0("/root/w2l/CTC/preOutput_0.txt");
      if(preOutFile_0.is_open())
      {
  preOutFile_0 << af::toString("preOutput_0 is:", softmax_add_preOutput.array());
  preOutFile_0.close();
      }
      
      
      ntwrk->train();
      crit->train();

      for (int i = 0; i < numNoise; i++) {
        printf("now training m%d\n",i);
        LOG(INFO) << "=================noise sample " << i << "==================";
        // meters
        af::sync();
        
        if (af::anyTrue<bool>(af::isNaN(pre_sample[kInputIdx])) ||
            af::anyTrue<bool>(af::isNaN(pre_sample[kTargetIdx]))) {
          LOG(FATAL) << "pre_sample has NaN values";
        }
//////////////////////////////////////////////////////////////////////////////////////////////////////
	//auto epsilon = (af::randn(noiseDims)) * 4268; 
        auto epsilon = fl::normal(noiseDims,fftstdev,0).array(); //add noises
	LOG(INFO)<<"epsilon mean is:"<<af::mean<float>(epsilon);
	LOG(INFO)<<"epsilon stdev is:"<<af::stdev<float>(epsilon);
	//save last iter epsilon parameter:
	if (i == numNoise-1)
	{
	   std::ofstream epsfile("/root/w2l/CTC/epsilon.txt");
	   if(epsfile.is_open())
  	   {
	      epsfile << af::toString("epsilon values:", epsilon);
              epsfile.close();
	   }
	}
        ///////////////////////////////////////////////////////////////////////////////////////////////
	auto rawinput = pre_sample[kFftIdx] + m * epsilon;
	//LOG(INFO)<<af::toString("epsilon 6 values:", epsilon(af::seq(6)));
	//LOG(INFO)<<af::toString("m 6 values:", m(af::seq(6)));
	//LOG(INFO)<<af::toString("rawinput 6 values:",rawinput(af::seq(6)));
        

        int T = noiseDims[1];
        int K = noiseDims[0]/2;
        af::array absinput(af::dim4(K, T, noiseDims[2], noiseDims[3]));
        af::array backinput(noiseDims);
        
        
        //LOG(INFO) << "m_epsilon mean :" << af::mean<float>(m*epsilon);
        //LOG(INFO) << "m_epsilon stdev :" << af::stdev<float>(m*epsilon);
        
        for (size_t j = 0; j < 2*K; j=j+2)
        {
            auto fir = rawinput(j, af::span, af::span, af::span);
            //LOG(INFO) << "fir row(i) dims is :" << fir.array().dims() << " " << af::toString("row(i) first value is ", fir.array()(0));
            auto sec = rawinput(j+1, af::span, af::span, af::span);
            //note shallow copy in fl::Variable
            auto temp = af::sqrt(fir * fir + sec * sec);
            absinput(j/2, af::span, af::span, af::span) =  temp;
            backinput(j, af::span, af::span, af::span) = temp;
            backinput(j+1, af::span, af::span, af::span) = temp;
        }

        //T x K x FLAGS_channels x batchSz
        af::array trInput = af::transpose(absinput);

        // dft kInputIdx not normalized
        //LOG(INFO) << "dft abs mean :" << af::mean<float>(absinput);
        //LOG(INFO) << "dft abs stdev :" << af::stdev<float>(absinput);

        // normalization
        auto mean = af::mean<float>(trInput); // along T and K two dimensions 1x1x1x1
        auto stdev = af::stdev<float>(trInput); //1 scalar
        auto finalInput = (trInput - mean) / stdev;
        fl::Variable trueInput(finalInput, true);
        
        auto indif = af::mean<float>(trInput - pre_sample[kInputIdx]);
        LOG(INFO) << "dft input difference mean is:" << indif;
        /*
        std::ofstream exfile("/home/zd/beforenorm.txt");
        if(exfile.is_open())
        {  
           exfile << af::toString("before norm", finalInput.array());
           exfile.close();
        }
        */

        // forward
        auto output = ntwrk->forward({trueInput}).front();
	auto output_arr = output.array();
	int tokendim=outputDims[0];	
	
	std::vector<int> axes1{1};
	fl::Variable tmpaddOutput = fl::sqrt(fl::var(output, axes1));
	tmpaddOutput = fl::tileAs(tmpaddOutput, output);
	fl::Variable addoutput = output/tmpaddOutput;

     //   for (size_t j = 0; j < tokendim; j=j+1)
      //{
        //  	auto framestdev=af::stdev<float>(output_arr(j,af::span,af::span,af::span));  
	//	output_arr(j,af::span,af::span,af::span)=output_arr(j,af::span,af::span,af::span)/framestdev;
          
      //}

        std::ofstream nowOutFile("/root/w2l/CTC/lastOutput.txt");
      if(nowOutFile.is_open())
      {
         nowOutFile<<af::toString("lastOutput is:", output_arr);
         nowOutFile.close();
      }
 //        af::array wgt = af::identity(31, 31); // numClasses are 31 tokens
	// wgt(0, 0) = 0;
 //        wgt(1, 1) = 0;
       
 //        wgt(28, 28) = 0;
 //        wgt(29, 29) = 0;
 //        wgt(30, 30) = 0;
 //        auto addweight = fl::Variable(wgt, true);
	      // auto addoutput = fl::matmul(addweight, output);
        //fl::Variable addoutput(output_arr,true);
	// auto softmax_output = fl::softmax(addoutput,1);
	auto tmp = addoutput(af::seq(2,27),af::span,af::span,af::span);
	auto softmax_add_output = fl::softmax(tmp,0);
	// addoutput(af::seq(2,27),af::span,af::span,af::span)=softmax_tmp; 
	// auto softmax_add_output = fl::matmul(addweight, addoutput);

        af::sync();
	if(i == numNoise-1)
	{
	    std::ofstream nowOutFile_0("/root/w2l/CTC/lastOutput_0.txt");
	    if(nowOutFile_0.is_open())
	    {
	       nowOutFile_0<<af::toString("lastOutput_0 is:", softmax_add_output.array());
	       nowOutFile_0.close();
	    }
  }

  if(i%1000 == 0)
  {
      char outdir[80];

      sprintf(outdir, "/root/w2l/CTC/music_mask_%d.txt", i);
  
      std::ofstream fft_mask_now(outdir);
      if(fft_mask_now.is_open())
      {
         fft_mask_now<<af::toString("mask music is:", rawinput);
         fft_mask_now.close();
      }
  }
        
        //LOG(INFO) << "network forward output dims is "<< output.array().dims();
        //LOG(INFO) << "load rawEmission preOutput dims is :" << preOutput.array().dims() ;
	float lambda = 0.1;
        //float lambda = 100;
        auto f_L2 = fl::norm(softmax_add_preOutput - softmax_add_output, {0,1});
        auto m_L2 = af::norm(m); //double
        auto myloss = f_L2 * f_L2;
        float m_mean=af::mean<float>(m);
        float m_var=af::var<float>(m);
  
        //auto firloss = fl::MeanSquaredError();
        //auto myloss = firloss(output, preOutput);

        float totloss = myloss.scalar<float>() - lambda * std::log(m_L2 * m_L2);

        LOG(INFO) << "f star norm is:" << af::norm(preOutput.array());
        LOG(INFO) << "f now norm is:" << af::norm(output.array());
        LOG(INFO) << "loss - f difference is :" << myloss.scalar<float>();
        LOG(INFO) << "loss - logm is :" << std::log(m_L2 * m_L2);
        LOG(INFO) << "loss is:" << totloss;
        Yfile << totloss << std::endl;
        Mlossfile << std::log(m_L2 * m_L2) << std::endl;
        Mmeanfile << m_mean<<std::endl;
        Mvarfile << m_var<<std::endl;
        mylossfile << myloss.scalar<float>()<<std::endl;


        af::sync();
       

        if (af::anyTrue<bool>(af::isNaN(myloss.array()))) {
          LOG(FATAL) << "Loss has NaN values";
        }

        // clear the gradients for next iteration
        netopt->zeroGrad();
        critopt->zeroGrad();
 //        zeroweight.zeroGrad();
	// addweight.zeroGrad();

        //Compute gradients using backprop
        myloss.backward();
        af::sync();
	//Print output's Grad
	if(i == numNoise-1)
	{
            std::ofstream outputGradFile("/root/w2l/CTC/outputGrad.txt");
	    if(outputGradFile.is_open())
	    {
	        outputGradFile << af::toString("output Grad is:", output.grad().array());
                outputGradFile.close();
	    }
  }

        if (FLAGS_maxgradnorm > 0) {
          auto params = ntwrk->params();
          if (clampCrit) {
            auto critparams = crit->params();
            params.insert(params.end(), critparams.begin(), critparams.end());
          }
          fl::clipGradNorm(params, FLAGS_maxgradnorm);
        }

        //critopt.step();
        //netopt.step();
        //update parameter m

        auto sigma2 = stdev * stdev;
        auto dy = trueInput.grad().array(); //T x K
        auto dsigma2 = af::sum<float>(dy * (trInput - mean) * (-0.5) * std::pow(sigma2, -1.5));
        auto dmu = af::sum<float>(dy * (-1.0/std::pow(sigma2, 0.5))) + af::sum<float>(-2 * (trInput - mean)) * dsigma2 / (T * K);
        auto dx = dy / std::pow(sigma2, 0.5) + dsigma2 * 2 * (trInput - mean) / (T * K) + dmu / (T * K); 

        af::array xGrad = af::transpose(dx); // K x T x 1 x 1
        auto midGrad = epsilon * epsilon * m + epsilon * pre_sample[kFftIdx];
        auto xGradm = midGrad / backinput; //2K x T x 1 x 1
        af::array mGrad = af::constant(0, noiseDims);

        // auto tmp = xGradm.dim();
        // printf("xGradm is %dx%dx%dx%d\n",tmp[0],tmp[1],tmp[2],tmp[3]);

        // tmp = xGrad.dim();
        // printf("xGrad is %dx%dx%dx%d\n",tmp[0],tmp[1],tmp[2],tmp[3]);

        for(size_t j=0; j< 2*K; j=j+2) {
          mGrad(j, af::span, af::span, af::span) = xGrad(j/2,af::span,af::span,af::span) * xGradm(j,af::span,af::span,af::span); 
          mGrad(j+1, af::span, af::span, af::span) = xGrad(j/2,af::span,af::span,af::span) * xGradm(j+1, af::span,af::span,af::span);
        }
	
        auto mGrad_aboutm_L2 = 2 * m / (m_L2 * m_L2);

        myloss_grad_mean_file << af::mean<float>(mGrad)<<std::endl;
        myloss_grad_var_file << af::var<float>(mGrad)<<std::endl;
        mloss_grad_mean_file << af::mean<float>(mGrad_aboutm_L2)<<std::endl;
        mloss_grad_var_file << af::var<float>(mGrad_aboutm_L2)<<std::endl;


        mGrad = mGrad - lambda * mGrad_aboutm_L2;

        m = m - mylr * mGrad;
        
        
        //network params whether to be changed
        fl::MSEMeter mymeter;
        auto psize = ntwrk->params().size();
        for(int j=0 ; j<psize; j++) {
          mymeter.add(ntwrk->param(j).array(), pretrained_params[j].array());
        }
        LOG(INFO) << "the network params change " << mymeter.value();        
      }


      af::sync();
      if (FLAGS_reportiters == 0) {
        //runValAndSaveModel(curEpoch, netopt->getLr(), critopt->getLr());
        //std::string mpath = "/root/w2l/aboutM/last_m.bin";
        //W2lSerializer::save(mpath, m);

        std::ofstream mfile("/root/w2l/CTC/lastm.txt");
        if(mfile.is_open())
        {
          mfile << af::toString("last m is:", m);
          mfile.close();
        }
      }
    }
  };

  /* ===================== Train ===================== */
  train(
      network,
      criterion,
      trainds,
      netoptim,
      critoptim,
      FLAGS_lr,
      FLAGS_lrcrit,
      true /* clampCrit */,
      FLAGS_iter);

  LOG_MASTER(INFO) << "Finished my training";
  return 0;
}
