// Compile syntax:
// > g++ server2.cpp -lboost_program_options-mt -lboost_thread -luhd -o server2

/* A simple server in the internet domain using TCP
   The port number is passed as an argument
   This version runs forever, forking off a separate
   process for each connection
*/



// =====================================================================================
// Includes and namespaces
// =====================================================================================
#include <iostream>
#include <fstream>
#include <vector>
#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>
#include <boost/thread/thread.hpp>
#include <boost/format.hpp>
#include <boost/math/special_functions/round.hpp>
#include <uhd/utils/thread_priority.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include "log4cxx/logger.h"
#include "log4cxx/propertyconfigurator.h"
#include "log4cxx/helpers/exception.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <pthread.h>
#include <fftw3.h>
#include "oml2/omlc.h"

namespace po = boost::program_options;

using namespace log4cxx;
using namespace log4cxx::helpers;

LoggerPtr logger(Logger::getLogger("uhd_daemon"));


// =====================================================================================
// OML class definitions
// =====================================================================================

#define FALSE (unsigned int) 0
#define TRUE  (unsigned int) 1

#define RECORD_TO_SERVER 1
#define RECORD_TO_FILE 2

#define USE_BLOB // FRUITFULLY USED WITH RECORD_TO_SERVER

class CWriteOML
{

private:

  unsigned int mMeasurementPoints;
  unsigned int mHeaderPoints;
  unsigned int mFFTLength;
  unsigned int mIdxNull;
  unsigned int mRecordMode;
  unsigned int mUseBlob;
  std::string mHostName;

  OmlMPDef* mp_def;
  OmlMP* mp;

  void createMeasurementPoint(OmlMPDef* pOmlMPDef, std::string str, OmlValueT type)
  {
    char* cptr;
    if (str == "NULL")
      {
        pOmlMPDef->name = NULL;
        pOmlMPDef->param_types = type;

      }
    else
      {
        cptr = new char[str.size()+1];
        strcpy (cptr, str.c_str());
        pOmlMPDef->name = cptr;
        pOmlMPDef->param_types = type;
      }
  }

public:

  CWriteOML()
  {
  }

  CWriteOML(std::string db_filename, std::string server_name)
  {
    init(db_filename, server_name);
  }



  void init(std::string db_filename, std::string server_name)
  {
    char chostname[32];
    std::string fname;
    int argc;
    const char** argv;

    for (int i = 0; i < 32;++i)
      chostname[i] = '\0';

    gethostname(chostname, 31);
    mHostName = std::string(chostname);

    std::string mode(server_name.c_str());

    if (mode == "file")
      {
	mRecordMode = RECORD_TO_FILE;
	mUseBlob = FALSE;
	fname = db_filename + "_" +  mHostName;
	argc = 7;
	const char* argv_file[] = {"./spectrum", "--oml-id",(const char*)chostname, "--oml-exp-id",db_filename.c_str(), "--oml-file",fname.c_str()};
	argv = argv_file;
      }
    else
      {
	mRecordMode = RECORD_TO_SERVER;
#ifdef USE_BLOB
	mUseBlob = TRUE;
#else
	mUseBlob = FALSE;
#endif
	argc = 7;
	const char* argv_server[] = {"./spectrum", "--oml-id",(const char*)chostname, "--oml-exp-id",db_filename.c_str(), "--oml-server", server_name.c_str()
	};
	argv = argv_server;
      }

    int result = omlc_init ("spectrum", &argc, argv, NULL);
    if (result == -1) {
      std::cerr << "Could not initialize OML\n";
      exit (1);
    }
  }

  void start(unsigned int points)
  {
    int result;

    mHeaderPoints = 5;
    mFFTLength = points;

    if (mUseBlob == TRUE)
      mMeasurementPoints = mHeaderPoints + 1;
    else
      mMeasurementPoints = mHeaderPoints + (1*mFFTLength);

    mIdxNull = mMeasurementPoints;

    mp_def = new OmlMPDef [(sizeof(OmlMPDef) * (mMeasurementPoints+1) )];

    // mHeaderPoints
    createMeasurementPoint(&mp_def[0], "sampling",    (OmlValueT)OML_INT32_VALUE);
    createMeasurementPoint(&mp_def[1], "cfreq_MHz",   (OmlValueT)OML_DOUBLE_VALUE);
    createMeasurementPoint(&mp_def[2], "gain_dB",     (OmlValueT)OML_INT32_VALUE);
    createMeasurementPoint(&mp_def[3], "FFTLength",   (OmlValueT)OML_INT32_VALUE);
    createMeasurementPoint(&mp_def[4], "FFTNum",      (OmlValueT)OML_STRING_VALUE);

    // mFFTLength
    if (mUseBlob == TRUE)
      {
        createMeasurementPoint(&mp_def[5], "FFTBins",     (OmlValueT)OML_BLOB_VALUE);
      }
    else
      {
        for (unsigned int i = mHeaderPoints; i < mIdxNull; ++i)
	  {
	    std::string str = "bin_" + boost::lexical_cast<std::string>(i-mHeaderPoints);
	    createMeasurementPoint(&mp_def[i], (char*)str.c_str(),    (OmlValueT)OML_DOUBLE_VALUE);
	  }
      }

    createMeasurementPoint(&mp_def[mIdxNull], "NULL",        (OmlValueT)0);

    mp = omlc_add_mp ("data", mp_def);

    if (mp == NULL) {
      std::cerr << "Error: could not register Measurement Point \"data\"";
      exit (1);
    }

    result = omlc_start();
    if (result == -1) {
      std::cerr << "Error starting up OML measurement streams\n";
      exit (1);
    }
  }

  void insert(uint32_t samp, float cf, uint32_t gain, char* text, float* buff)
  {
    OmlValueU values[mMeasurementPoints];
    float* fptr = buff;

    omlc_set_long   (values[0], samp);
    omlc_set_double (values[1], cf);
    omlc_set_long   (values[2], gain);
    omlc_set_long   (values[3], mFFTLength);
    omlc_set_string (values[4], text);

    if (mUseBlob == TRUE)
      {
	values[5].blobValue.data = NULL;
	values[5].blobValue.size = 0;
	omlc_set_blob (values[5], (char *)fptr, 1*mFFTLength*sizeof(float));
	//std::cerr << *fptr << std::endl;
      }
    else
      {
	for (unsigned int i = mHeaderPoints; i < mIdxNull; ++i)
	  {
	    omlc_set_double (values[i], *fptr++);
	  }
      }

    omlc_inject (mp, values);
  }

  void stop()
  {
    omlc_close();
  }

};

/***********************************************************************
 * Waveform generators
 **********************************************************************/
static const size_t wave_table_len = 8192;

class wave_table_class{
public:
  wave_table_class(const std::string &wave_type, const float ampl):
    _wave_table(wave_table_len)
  {
    //compute real wave table with 1.0 amplitude
    std::vector<double> real_wave_table(wave_table_len);
    if (wave_type == "CONST"){
      for (size_t i = 0; i < wave_table_len; i++)
	real_wave_table[i] = 1.0;
    }
    else if (wave_type == "SQUARE"){
      for (size_t i = 0; i < wave_table_len; i++)
	real_wave_table[i] = (i < wave_table_len/2)? 0.0 : 1.0;
    }
    else if (wave_type == "RAMP"){
      for (size_t i = 0; i < wave_table_len; i++)
	real_wave_table[i] = 2.0*i/(wave_table_len-1) - 1.0;
    }
    else if (wave_type == "SINE"){
      static const double tau = 2*std::acos(-1.0);
      for (size_t i = 0; i < wave_table_len; i++)
	real_wave_table[i] = std::sin((tau*i)/wave_table_len);
    }
    else throw std::runtime_error("unknown waveform type: " + wave_type);

    //compute i and q pairs with 90% offset and scale to amplitude
    for (size_t i = 0; i < wave_table_len; i++){
      const size_t q = (i+(3*wave_table_len)/4)%wave_table_len;
      _wave_table[i] = std::complex<float>(ampl*real_wave_table[i], ampl*real_wave_table[q]);
    }
  }

  inline std::complex<float> operator()(const size_t index) const{
    return _wave_table[index % wave_table_len];
  }

private:
  std::vector<std::complex<float> > _wave_table;
};


// =====================================================================================
// Global variables
// =====================================================================================
struct tConfigParameters { 
  unsigned int numbins;    // RX: number of fft points
  unsigned int avgwinlen;  // RX: moving average window size
  std::string wavetype;    // TX: wave type
  std::string otw;         // TX: over-the-air sample mode
  double wavefreq;         // TX: wafe frequency
  float txwaveampl;        // TX: waveform amplitude
  size_t spb;              // TX: samples per buffer
  std::string omlDbFilename;
  std::string omlServerName;
};


struct tConfigParameters gCP;
uhd::usrp::multi_usrp::sptr usrp;

pthread_t gPThread;
unsigned int gThreadRunningFlag;




// =====================================================================================
// Function prototypes
// =====================================================================================
int parse_command(int); /* function prototype */


void error(const char *msg)
{
  perror(msg);
  exit(1);
}


int main(int argc, char *argv[])
{
  uhd::set_thread_priority_safe();

  std::string args, ant, subdev, ref;
  struct sockaddr_in serv_addr, cli_addr;

  int sockfd, portno, newsockfd;
  double rate, freq, gain, bw;
  socklen_t clilen;

  //setup the program options
  po::options_description desc("Allowed options");
  desc.add_options()
    ("help", "brief description of get/set handlers")
    ("args", po::value<std::string>(&args)->default_value(""), "multi uhd device address args")
    ("port", po::value<int>(&portno)->default_value(5123), "specify port to communicate on")

    // hardware parameters
    ("rate", po::value<double>(&rate)->default_value(8e6), "rate of incoming samples (sps)")
    ("freq", po::value<double>(&freq)->default_value(2450e6), "RF center frequency in Hz")
    ("gain", po::value<double>(&gain)->default_value(0.0), "gain for the RF chain")
    ("ant", po::value<std::string>(&ant), "daughterboard antenna selection")
    ("subdev", po::value<std::string>(&subdev), "daughterboard subdevice specification")
    ("bw", po::value<double>(&bw), "daughterboard IF filter bandwidth in Hz")
    ("ref", po::value<std::string>(&ref)->default_value("internal"), "waveform type (internal, external, mimo)")

    ("num-bins", po::value<unsigned int>(&gCP.numbins)->default_value(256), "the number of FFT points")
    ("win-size", po::value<size_t>(&gCP.avgwinlen)->default_value(64), "moving average window size for FFT bins")
    ("wave-type", po::value<std::string>(&gCP.wavetype)->default_value("CONST"), "waveform type (CONST, SQUARE, RAMP, SINE)")
    ("wave-freq", po::value<double>(&gCP.wavefreq)->default_value(0), "waveform frequency in Hz")
    ("otw", po::value<std::string>(&gCP.otw)->default_value("sc16"), "specify the over-the-wire sample mode")
    ("ampl", po::value<float>(&gCP.txwaveampl)->default_value(float(0.3)), "amplitude of the waveform [0 to 0.7]")
    ("spb", po::value<size_t>(&gCP.spb)->default_value(0), "samples per buffer, 0 for default")

    ("oml-exp-id", po::value<std::string>(&gCP.omlDbFilename)->default_value("spectrum"), "OML database file to log data")
    ("oml-server", po::value<std::string>(&gCP.omlServerName)->default_value("idb2:3003"), "OML server to send log data: [idb2:3003,localhost:3003]")
     ;

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  //print the help message
  if (vm.count("help")){
    std::cout << boost::format("%s") % desc << std::endl;
    return ~0;
  }

  // Configure the logger
  PropertyConfigurator::configure("/etc/uhd_logconf.prof");



  // =====================================================================================
  // Set up socket connection
  // =====================================================================================

  if ( (sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
  {
    LOG4CXX_ERROR(logger, "Unable to open socket");
    error("ERROR opening socket");
  }
  bzero((char *) &serv_addr, sizeof(serv_addr));

  int so_reuseaddr = TRUE;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof so_reuseaddr) != 0)
  {
    LOG4CXX_ERROR(logger, "Unable to set socket options");
    error("ERROR setting socket options");
  }

  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(portno);
  if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
  {
    LOG4CXX_ERROR(logger, "Unable to bind to portno " << portno);
    error("ERROR on bind to port");
  }
  listen(sockfd,2);
  clilen = sizeof(cli_addr);

  LOG4CXX_INFO(logger, "started on portno:" << portno);


  // =====================================================================================
  // Create USRP Device
  // =====================================================================================
  std::cout << std::endl;
  std::cout << boost::format("Creating the usrp device with: %s...") % args << std::endl;
  usrp = uhd::usrp::multi_usrp::make(args);

  // Lock mboard clocks
  usrp->set_clock_source(ref);


  // =====================================================================================
  // Set up uhd rx chain
  // =====================================================================================

  //always select the subdevice first, the channel mapping affects the other settings
  if (vm.count("subdev")) usrp->set_rx_subdev_spec(subdev);

  std::cout << boost::format("Using Device: %s") % usrp->get_pp_string() << std::endl;

  //set the sample rate
  std::cout << boost::format("Setting RX Rate: %f Msps...") % (rate/1e6) << std::endl;
  usrp->set_rx_rate(rate);
  std::cout << boost::format("Actual RX Rate: %f Msps...") % (usrp->get_rx_rate()/1e6) << std::endl << std::endl;
  //rate = usrp->get_rx_rate();

  //set the center frequency
  std::cout << boost::format("Setting RX Freq: %f MHz...") % (freq/1e6) << std::endl;
  usrp->set_rx_freq(freq);
  std::cout << boost::format("Actual RX Freq: %f MHz...") % (usrp->get_rx_freq()/1e6) << std::endl << std::endl;

  //set the rf gain
  if (vm.count("gain"))
  {
    std::cout << boost::format("Setting RX Gain: %f dB...") % gain << std::endl;
    usrp->set_rx_gain(gain);
    std::cout << boost::format("Actual RX Gain: %f dB...") % usrp->get_rx_gain() << std::endl << std::endl;
  }

  //set the IF filter bandwidth
  if (vm.count("bw"))
  {
    std::cout << boost::format("Setting RX Bandwidth: %f MHz...") % bw << std::endl;
    usrp->set_rx_bandwidth(bw);
    std::cout << boost::format("Actual RX Bandwidth: %f MHz...") % usrp->get_rx_bandwidth() << std::endl << std::endl;
  }

  //set the antenna
  if (vm.count("ant")) usrp->set_rx_antenna("RX2");

  boost::this_thread::sleep(boost::posix_time::seconds(1)); //allow for some setup time


  //Check Ref and LO Lock detect
  {
  std::vector<std::string> sensor_names;
  sensor_names = usrp->get_rx_sensor_names(0);
  if (std::find(sensor_names.begin(), sensor_names.end(), "lo_locked") != sensor_names.end()) {
    uhd::sensor_value_t lo_locked = usrp->get_rx_sensor("lo_locked",0);
    std::cout << boost::format("Checking RX: %s ...") % lo_locked.to_pp_string() << std::endl;
    UHD_ASSERT_THROW(lo_locked.to_bool());
  }
  sensor_names = usrp->get_mboard_sensor_names(0);
  if ((ref == "mimo") and (std::find(sensor_names.begin(), sensor_names.end(), "mimo_locked") != sensor_names.end()))
  {
    uhd::sensor_value_t mimo_locked = usrp->get_mboard_sensor("mimo_locked",0);
    std::cout << boost::format("Checking RX: %s ...") % mimo_locked.to_pp_string() << std::endl;
    UHD_ASSERT_THROW(mimo_locked.to_bool());
  }
  if ((ref == "external") and (std::find(sensor_names.begin(), sensor_names.end(), "ref_locked") != sensor_names.end()))
  {
    uhd::sensor_value_t ref_locked = usrp->get_mboard_sensor("ref_locked",0);
    std::cout << boost::format("Checking RX: %s ...") % ref_locked.to_pp_string() << std::endl;
    UHD_ASSERT_THROW(ref_locked.to_bool());
  }
  }
  // =====================================================================================
  // Set up uhd tx chain
  // =====================================================================================

  //always select the subdevice first, the channel mapping affects the other settings
  if (vm.count("subdev")) usrp->set_tx_subdev_spec(subdev);

  std::cout << boost::format("Using Device: %s") % usrp->get_pp_string() << std::endl;

  //set the sample rate
  std::cout << boost::format("Setting TX Rate: %f Msps...") % (rate/1e6) << std::endl;
  usrp->set_tx_rate(rate);
  std::cout << boost::format("Actual TX Rate: %f Msps...") % (usrp->get_tx_rate()/1e6) << std::endl << std::endl;
  //rate = usrp->get_tx_rate();

  //set the center frequency
  std::cout << boost::format("Setting TX Freq: %f MHz...") % (freq/1e6) << std::endl;
  usrp->set_tx_freq(freq);
  std::cout << boost::format("Actual TX Freq: %f MHz...") % (usrp->get_tx_freq()/1e6) << std::endl << std::endl;

  //set the rf gain
  if (vm.count("gain"))
  {
    std::cout << boost::format("Setting TX Gain: %f dB...") % gain << std::endl;
    usrp->set_tx_gain(gain);
    std::cout << boost::format("Actual TX Gain: %f dB...") % usrp->get_tx_gain() << std::endl << std::endl;
  }

  //set the IF filter bandwidth
  if (vm.count("bw"))
  {
    std::cout << boost::format("Setting TX Bandwidth: %f MHz...") % bw << std::endl;
    usrp->set_tx_bandwidth(bw);
    std::cout << boost::format("Actual TX Bandwidth: %f MHz...") % usrp->get_tx_bandwidth() << std::endl << std::endl;
  }

  //set the antenna
  if (vm.count("ant")) usrp->set_tx_antenna("TX/RX");

  boost::this_thread::sleep(boost::posix_time::seconds(1)); //allow for some setup time


  //Check Ref and LO Lock detect
  {
  std::vector<std::string> sensor_names;
  sensor_names = usrp->get_tx_sensor_names(0);
  if (std::find(sensor_names.begin(), sensor_names.end(), "lo_locked") != sensor_names.end()) {
    uhd::sensor_value_t lo_locked = usrp->get_tx_sensor("lo_locked",0);
    std::cout << boost::format("Checking TX: %s ...") % lo_locked.to_pp_string() << std::endl;
    UHD_ASSERT_THROW(lo_locked.to_bool());
  }
  sensor_names = usrp->get_mboard_sensor_names(0);
  if ((ref == "mimo") and (std::find(sensor_names.begin(), sensor_names.end(), "mimo_locked") != sensor_names.end()))
  {
    uhd::sensor_value_t mimo_locked = usrp->get_mboard_sensor("mimo_locked",0);
    std::cout << boost::format("Checking TX: %s ...") % mimo_locked.to_pp_string() << std::endl;
    UHD_ASSERT_THROW(mimo_locked.to_bool());
  }
  if ((ref == "external") and (std::find(sensor_names.begin(), sensor_names.end(), "ref_locked") != sensor_names.end()))
  {
    uhd::sensor_value_t ref_locked = usrp->get_mboard_sensor("ref_locked",0);
    std::cout << boost::format("Checking TX: %s ...") % ref_locked.to_pp_string() << std::endl;
    UHD_ASSERT_THROW(ref_locked.to_bool());
  }
  }

  // =====================================================================================
  // Set device timestamp
  // =====================================================================================
  std::cout << boost::format("Setting device timestamp to 0...") << std::endl;
  usrp->set_time_now(uhd::time_spec_t(0.0));

  // =====================================================================================
  // main loop
  // =====================================================================================

  std::cout << "Server ready on port number: " << portno << std::endl;
  while (1)
  {
    int ret_code;

    // Wait and accept connection on newsockfd
    newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
    if (newsockfd < 0) error("ERROR on accept");
    
    ret_code = parse_command(newsockfd);
    close(newsockfd); // For parent process close child newsockfd

    if (ret_code != 0) break;

  } /* end of while */

  close(sockfd);
  std::cout << "Done!" << std::endl;

  return 0; /* we never get here */
}



// =====================================================================================
// Thread: RX Peak 
// =====================================================================================
struct tThreadArgs {
  int mode;
  int sockfd, portno;
};

void *pth_rxpeak(void *arglist)
{
  unsigned int nCount = 0;

  //create a receive streamer
  uhd::stream_args_t stream_args("fc32"); //complex floats
  uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

  uhd::rx_metadata_t md;
  std::vector<std::complex<float> > buff(gCP.numbins);
  std::vector<std::complex<float> > out_buff(gCP.numbins);
  std::vector<float > rbuff(gCP.numbins);
  struct tThreadArgs *ta = (struct tThreadArgs *)arglist;

  // set up udp server
  int sockfd=0, newsockfd=0, n;
  socklen_t clilen;
  struct sockaddr_in serv_addr, cli_addr;
  char buffer[1024];

  if (ta->mode == 1)
  {
    sockfd=socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("Opening socket");

    bzero(&serv_addr,sizeof(serv_addr));

    serv_addr.sin_family=AF_INET;
    serv_addr.sin_addr.s_addr=INADDR_ANY;
    serv_addr.sin_port=htons(ta->portno );
    if (bind(sockfd,(struct sockaddr *)&serv_addr, sizeof(serv_addr))<0)
      error("binding");

    listen(sockfd,2);
    clilen = sizeof(cli_addr);
    newsockfd = accept(sockfd,(struct sockaddr *) &cli_addr,&clilen);
    if (newsockfd < 0)
      error("ERROR on accept");
    bzero(buffer,256);
    n = read(newsockfd,buffer,255); 
    if (n < 0) error("ERROR reading from socket");

  }

  // set up FFT engine
  fftwf_complex *in = (fftwf_complex*)&buff.front();
  fftwf_complex *out = (fftwf_complex*)&out_buff.front();
  fftwf_plan p;
  p = fftwf_plan_dft_1d(gCP.numbins, in, out, FFTW_FORWARD, FFTW_ESTIMATE);


  usrp->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
  boost::system_time next_refresh = boost::get_system_time();

  gThreadRunningFlag = 1;
  while( gThreadRunningFlag == 1)
  {

    //read a buffer's worth of samples every iteration
    size_t num_rx_samps = rx_stream->recv(&buff.front(), buff.size(), md);
    //std::cerr << nCount << "  ";
    if (num_rx_samps != buff.size())
      {
	std::cerr << num_rx_samps << " " << md.error_code << std::endl;
        continue ;
      }

    //check and update the refresh condition - vile indeed... I put it down here and it quiets it down
    if (boost::get_system_time() < next_refresh) { continue; }
    next_refresh = boost::get_system_time() + boost::posix_time::microseconds(long(1e6*gCP.numbins / usrp->get_rx_rate() ));


    nCount++;
    //fftwf_execute(p);

    if ((nCount % gCP.avgwinlen) != 0) { continue; }

    for (unsigned int i = 0; i < rbuff.size(); ++i)
      rbuff.at(i) = buff.at(i).real();

    n=write(newsockfd,(char *)&rbuff.front(),sizeof(float)*rbuff.size());
    if (n  < 0) error("write");
    std::cout << nCount << std::endl;

  } // While()


  fftwf_destroy_plan(p);
  usrp->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);

  return NULL;
}


void *pth_rxstream_to_oml(void *arglist)
{
  unsigned int nCount = 0;

  //create a receive streamer
  uhd::stream_args_t stream_args("fc32"); //complex floats
  uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

  //allocate recv buffer and metatdata
  uhd::rx_metadata_t md;
  std::vector<std::complex<float> > buff(gCP.numbins);
  std::vector<std::complex<float> > out_buff(gCP.numbins);
  std::vector<float> mag_buff(gCP.numbins);
  struct tThreadArgs *ta = (struct tThreadArgs *)arglist;

  // allocate OML resources
  CWriteOML OML(gCP.omlDbFilename, gCP.omlServerName);
  OML.start(gCP.numbins);
  LOG4CXX_INFO(logger, "oml recording start -- " << gCP.omlDbFilename << ".sq3 @ " << gCP.omlServerName);

  // set up FFT engine
  fftwf_complex *in = (fftwf_complex*)&buff.front();
  fftwf_complex *out = (fftwf_complex*)&out_buff.front();
  fftwf_plan p;
  p = fftwf_plan_dft_1d(gCP.numbins, in, out, FFTW_FORWARD, FFTW_ESTIMATE);

  // config uhd modes
  usrp->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
  boost::system_time next_refresh = boost::get_system_time();

  gThreadRunningFlag = 1;
  while( gThreadRunningFlag == 1)
  {

    //read a buffer's worth of samples every iteration
    size_t num_rx_samps = rx_stream->recv(&buff.front(), buff.size(), md);
    //std::cerr << nCount << "  ";
    if (num_rx_samps != buff.size())
    {
	std::cerr << num_rx_samps << " " << md.error_code << std::endl;
	LOG4CXX_ERROR(logger, "uhd rx size mismatch: num_rx_samps, buff.size = " << num_rx_samps << ", " << buff.size() );
        continue ;
    }

    //check and update the refresh condition - vile indeed... I put it down here and it quiets it down
    if (boost::get_system_time() < next_refresh) { continue; }
    next_refresh = boost::get_system_time() + boost::posix_time::microseconds(long(1e6*gCP.numbins / usrp->get_rx_rate() ));

    nCount++;
    if ((nCount % 1000) != 0) { continue; }
    fftwf_execute(p);

    // find magnitude value
    for (unsigned int i = 0 ; i < gCP.numbins ; ++i)
      mag_buff.at(i) = abs( out_buff.at(i) ); // the abs() here takes the sqrt as well

    // dump out FD to file
    OML.insert((uint32_t)usrp->get_rx_rate(),
	       (float)usrp->get_rx_freq(),
	       (uint32_t)usrp->get_rx_gain(),
	       (char*)"---",
	       (float*)&mag_buff.front());

  } // While()


  fftwf_destroy_plan(p);
  usrp->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
  OML.stop();
  LOG4CXX_INFO(logger, "oml recording stop");

  return NULL;
} // pth_rxstream_to_oml()


// =====================================================================================
// Thread: TX stream
// =====================================================================================
void *pth_txstream(void *arg)
{
  //for the const wave, set the wave freq for small samples per period
  if (gCP.wavefreq == 0 and gCP.wavetype == "CONST"){
    gCP.wavefreq = usrp->get_tx_rate()/2;
  }

  //error when the waveform is not possible to generate
  if (std::abs(gCP.wavefreq) > usrp->get_tx_rate()/2){
    throw std::runtime_error("wave freq out of Nyquist zone");
  }
  if (usrp->get_tx_rate()/std::abs(gCP.wavefreq) > wave_table_len/2){
    throw std::runtime_error("wave freq too small for table");
  }

  //pre-compute the waveform values
  const wave_table_class wave_table(gCP.wavetype, gCP.txwaveampl);
  const size_t step = boost::math::iround(gCP.wavefreq/usrp->get_tx_rate() * wave_table_len);
  size_t index = 0;

  //create a transmit streamer
  uhd::stream_args_t stream_args("fc32", gCP.otw);
  uhd::tx_streamer::sptr tx_stream = usrp->get_tx_stream(stream_args);

  //allocate a buffer which we re-use for each channel
  if (gCP.spb == 0)
    gCP.spb = tx_stream->get_max_num_samps()*10;
  std::vector<std::complex<float> > buff(gCP.spb);
  std::vector<std::complex<float> *> buffs(usrp->get_tx_num_channels(), &buff.front());

  //setup the metadata flags
  uhd::tx_metadata_t md;
  md.start_of_burst = true;
  md.end_of_burst   = false;
  md.has_time_spec  = true;
  md.time_spec = uhd::time_spec_t(0.1);

  // =====================================================================================
  // Set device timestamp
  // =====================================================================================
  std::cout << boost::format("Setting device timestamp to 0...") << std::endl;
  usrp->set_time_now(uhd::time_spec_t(0.0));

  LOG4CXX_INFO(logger, "txstream started");

  gThreadRunningFlag = 1;

  //send data until the signal handler gets called
  while( gThreadRunningFlag == 1)
  {
    //fill the buffer with the waveform
    for (size_t n = 0; n < buff.size(); n++){
      buff[n] = wave_table(index += step);
    }

    //send the entire contents of the buffer
    tx_stream->send(buffs, buff.size(), md);

    md.start_of_burst = false;
    md.has_time_spec = false;
  }

  //send a mini EOB packet
  md.end_of_burst = true;
  tx_stream->send("", 0, md);

  LOG4CXX_INFO(logger, "txstream stop");

  return NULL;
}

// Valid command from uhd_cli.py
// set rxfreq 4000e6
// set rxrate 8e6
// set rxgain 15
// set numbins 256
// rxstream2oml
// stop
// test

int parse_command (int sock)
{
  int n, return_code = 0;
  char buffer[256];


  bzero(buffer,256);
  n = read(sock,buffer,255);

  if (n < 0) error("ERROR reading from socket");

  std::string temp(buffer);
  std::vector<std::string> sToken;
  boost::split(sToken, temp, boost::is_any_of(" ") );

  if (sToken.at(0) == "set")
  {
    temp = "set ";
    if (sToken.at(1) == "rxfreq")      // set rx carrier frequency
    {
      usrp->set_rx_freq( boost::lexical_cast<double>( sToken.at(2) ) );
      temp += sToken.at(1) + " " + boost::lexical_cast<std::string>( usrp->get_rx_freq() );
    }
    else if (sToken.at(1) == "rxrate") // set rx sample rate
    {
      usrp->set_rx_rate( boost::lexical_cast<double>( sToken.at(2) ) );
      temp += sToken.at(1) + " " + boost::lexical_cast<std::string>( usrp->get_rx_rate() );
    }
    else if (sToken.at(1) == "rxgain") // set rx path gain
    {
      usrp->set_rx_gain( boost::lexical_cast<double>( sToken.at(2) ) );
      temp += sToken.at(1) + " " + boost::lexical_cast<std::string>( usrp->get_rx_gain() );
    }
    else if (sToken.at(1) == "numbins") // set number of fft points
    {
      gCP.numbins = boost::lexical_cast<double>( sToken.at(2) );
      temp += sToken.at(1) + " " + boost::lexical_cast<std::string>( gCP.numbins );
    }
    else if (sToken.at(1) == "avgwinlen") // set averaging window size
    {
      gCP.avgwinlen = boost::lexical_cast<double>( sToken.at(2) );
      temp += sToken.at(1) + " " + boost::lexical_cast<std::string>( gCP.numbins );
    }
    else if (sToken.at(1) == "txfreq")      // set tx carrier frequency
    {
      usrp->set_tx_freq( boost::lexical_cast<double>( sToken.at(2) ) );
      temp += sToken.at(1) + " " + boost::lexical_cast<std::string>( usrp->get_tx_freq() );
    }
    else if (sToken.at(1) == "txrate") // set tx sample rate
    {
      usrp->set_tx_rate( boost::lexical_cast<double>( sToken.at(2) ) );
      temp += sToken.at(1) + " " + boost::lexical_cast<std::string>( usrp->get_tx_rate() );
    }
    else if (sToken.at(1) == "txgain") // set tx path gain
    {
      usrp->set_tx_gain( boost::lexical_cast<double>( sToken.at(2) ) );
      temp += sToken.at(1) + " " + boost::lexical_cast<std::string>( usrp->get_tx_gain() );
    }
    else if (sToken.at(1) == "txampl") // set tx generated waveform amplitude
    {
      gCP.txwaveampl = boost::lexical_cast<float>( sToken.at(2) );
      temp += sToken.at(1) + " " + boost::lexical_cast<std::string>( gCP.txwaveampl );
    }
    else if (sToken.at(1) == "txwavetype") // set tx generate waveform type = {CONST, SINE, SQUARE, RAMP}
    {
      gCP.wavetype = boost::lexical_cast<std::string>( sToken.at(2) );
      temp += sToken.at(1) + " " + boost::lexical_cast<std::string>( gCP.wavetype );
    }
    else if (sToken.at(1) == "txwavefreq") // set tx generated tx wavefrom frequency
    {
      gCP.wavefreq = boost::lexical_cast<double>( sToken.at(2) );
      temp += sToken.at(1) + " " + boost::lexical_cast<std::string>( gCP.wavefreq );
    }
    else if (sToken.at(1) == "omlfile")
    {
      gCP.omlDbFilename =  sToken.at(2);
      temp += sToken.at(1) + " " + gCP.omlDbFilename;
    }
    else if (sToken.at(1) == "omlserver")
    {
      gCP.omlServerName =  sToken.at(2);
      temp += sToken.at(1) + " " + gCP.omlServerName;
    }
    else
    {
      temp += "unknown parameter";
    }
  }
  else if (sToken.at(0) == "get")
  {
    temp = "get ";
    if (sToken.at(1) == "rxfreq")      // get rx carrier frequency
    {
      temp = sToken.at(1) + " " + boost::lexical_cast<std::string>( usrp->get_rx_freq() );
    }
    else if (sToken.at(1) == "rxrate") // get rx sample rate
    {
      temp = sToken.at(1) + " " + boost::lexical_cast<std::string>( usrp->get_rx_rate() );
    }
    else if (sToken.at(1) == "rxgain") // get rx path gain
    {
      temp = sToken.at(1) + " " + boost::lexical_cast<std::string>( usrp->get_rx_gain() );
    }
    else if (sToken.at(1) == "numbins") // get number of fft points
    {
      temp = sToken.at(1) + " " + boost::lexical_cast<std::string>( gCP.numbins );
    }
    else if (sToken.at(1) == "avgwinlen") // get averaging window size
    {
      temp = sToken.at(1) + " " + boost::lexical_cast<std::string>( gCP.avgwinlen );
    }
    else if (sToken.at(1) == "omlfile")
    {
      temp = sToken.at(1) + " " + gCP.omlDbFilename;
    }
    else if (sToken.at(1) == "omlserver")
    {
      temp = sToken.at(1) + " " + gCP.omlServerName;
    }
    else
    {
      temp += "unknown parameter";
    }
  }
  else if (sToken.at(0) == "quit")
  {
    return_code = 1;
  }
  else if (sToken.at(0) == "rxpeak")
  {
    struct tThreadArgs ta;
    ta.mode = 1;
    ta.portno = 5100;
    pthread_create(&gPThread,NULL,pth_rxpeak,(void*)&ta);
  }
  else if (sToken.at(0) == "rxstream2oml")
  {
    struct tThreadArgs ta;
    ta.mode = 1;
    ta.portno = 5100;
    pthread_create(&gPThread,NULL,pth_rxstream_to_oml,(void*)&ta);
  }
  else if (sToken.at(0) == "txstream")
  {
    pthread_create(&gPThread,NULL,pth_txstream,(void*)"foo");
  }
  else if (sToken.at(0) == "stop")
  {
    gThreadRunningFlag = 0;
  }
  else if (sToken.at(0) == "test")
  {
    temp += " daemon alive!";
  }
  else
  {
    temp = "Unknown command";
  }

  //std::cout << sToken.at(0) << std::endl;
  //std::cout << sToken.at(1) << std::endl;
  //std::cout << sToken.at(2) << std::endl;
  
  LOG4CXX_INFO(logger, temp);

  std::cout << buffer << std::endl;
  n = write(sock, temp.c_str(), temp.length());

  if (n < 0)
    error("ERROR writing to socket");

    return (return_code);
}


