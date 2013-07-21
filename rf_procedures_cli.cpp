// 2013
/* rf_procedures_cli is a simple program for spectrum sensing using USRP and uhd 
// more explainations in the comments
// if the chnsts mode display is kept, need ncurses.h installed and use -lncurses when compile along with boost and uhd required libs
// data is send to udp connection*/ 


#include <uhd/utils/thread_priority.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/transport/udp_simple.hpp>
#include <uhd/exception.hpp>
#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <boost/lexical_cast.hpp>
#include <iostream>
#include <complex>
#include <csignal>
#include <iomanip>
#include <fftw3.h>
#include <numeric>
#include <curses.h>
//#include <algorithm>
#include <iterator>
#include <string>

namespace po = boost::program_options;
static bool stop_signal_called = false;
void sig_int_handler(int){stop_signal_called = true;}


// Moving average function for avmfft mode
std::vector<float> Moving_Avg(std::vector<std::complex<float> > fft_data, unsigned int nWindow_size) {
     std::vector<float> ret_vect;
      for (unsigned int i=0; i<fft_data.size()-nWindow_size;++i)
      {float cusum=0;
  	for (unsigned int j=i-1;j<i-1+nWindow_size;++j)
		cusum+=abs(fft_data[j]);
		ret_vect.push_back (cusum/nWindow_size);
		//std::cout<< cusum/Window_size<<std::endl; 
	  }
return ret_vect;
}

int UHD_SAFE_MAIN(int argc, char *argv[]){
    uhd::set_thread_priority_safe();

    //variables to be set by po
    std::string args, file, ant, subdev, ref;
    size_t total_num_samps;
    size_t num_bins;
    unsigned int Moving_Avg_size, navrg;
    double rate, freq, gain, bw,chbw;
    std::string addr, port, mode;
    
    // This for "chnsts" mode, for test purposes we will use this threshold value and can be adjusted as required. 
    // More work is needed to compute threshold based on USRP noise figure, gain and even center freq
    // because noise figure changes with freq
    double thresh=0.0015;
    
    //setup the program options
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "help message")
        ("args", po::value<std::string>(&args)->default_value(""), "multi uhd device address args")
        ("nsamps", po::value<size_t>(&total_num_samps)->default_value(0), "Total number of samples to receive, zero for continuous mode")
        ("rate", po::value<double>(&rate)->default_value(2e6), "rate of incoming samples")
        ("freq", po::value<double>(&freq)->default_value(400e6), "rf center frequency in Hz")
        ("gain", po::value<double>(&gain)->default_value(0), "gain for the RF chain")
        ("ant", po::value<std::string>(&ant), "daughterboard antenna selection")
        ("subdev", po::value<std::string>(&subdev), "daughterboard subdevice specification")
        ("bw", po::value<double>(&bw), "daughterboard IF filter bandwidth in Hz")
        ("port", po::value<std::string>(&port)->default_value("7124"), "server udp port")
        ("addr", po::value<std::string>(&addr)->default_value("192.168.1.10"), "resolvable server address")
        ("mode", po::value<std::string>(&mode)->default_value("avmfft"), "Default avmfft, others cmpfft, tmsmps, chnsts")
        ("chbw", po::value<double>(&chbw)->default_value(200e3), "Channel bandwidith for Chsts mode")
        ("navrg", po::value<unsigned int>(&navrg)->default_value(5), "Number of average times for chnsts mode")
        ("ref", po::value<std::string>(&ref)->default_value("internal"), "waveform type (internal, external, mimo)")
        ("num-bins", po::value<size_t>(&num_bins)->default_value(1024), "the number of FFT points")
        ("win-size", po::value<size_t>(&Moving_Avg_size)->default_value(4), "moving average window size for FFT bins across time")
    ;
      
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    //print the help message
    if (vm.count("help")){
        std::cout << boost::format("UHD RX to UDP %s") % desc << std::endl;
        return ~0;
    }

    //create a usrp device
    std::cout << std::endl;
    std::cout << boost::format("Creating the usrp device with: %s...") % args << std::endl;
    uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(args);
    std::cout << boost::format("Using Device: %s") % usrp->get_pp_string() << std::endl;

    //Lock mboard clocks
    usrp->set_clock_source(ref);

    //set the rx sample rate
    std::cout << boost::format("Setting RX Rate: %f Msps...") % (rate/1e6) << std::endl;
    usrp->set_rx_rate(rate);
    std::cout << boost::format("Actual RX Rate: %f Msps...") % (usrp->get_rx_rate()/1e6) << std::endl << std::endl;

    //set the rx center frequency
    std::cout << boost::format("Setting RX Freq: %f Mhz...") % (freq/1e6) << std::endl;
    usrp->set_rx_freq(freq);
    std::cout << boost::format("Actual RX Freq: %f Mhz...") % (usrp->get_rx_freq()/1e6) << std::endl << std::endl;

    //set the rx rf gain
    std::cout << boost::format("Setting RX Gain: %f dB...") % gain << std::endl;
    usrp->set_rx_gain(gain);
    std::cout << boost::format("Actual RX Gain: %f dB...") % usrp->get_rx_gain() << std::endl << std::endl;

    //set the IF filter bandwidth
    if (vm.count("bw")){
        std::cout << boost::format("Setting RX Bandwidth: %f MHz...") % bw << std::endl;
        usrp->set_rx_bandwidth(bw);
        std::cout << boost::format("Actual RX Bandwidth: %f MHz...") % usrp->get_rx_bandwidth() << std::endl << std::endl;
    }

    //set the antenna
    if (vm.count("ant")) usrp->set_rx_antenna(ant);

    boost::this_thread::sleep(boost::posix_time::seconds(1)); //allow for some setup time

    //Check Ref and LO Lock detect
    std::vector<std::string> sensor_names;
    sensor_names = usrp->get_rx_sensor_names(0);
    if (std::find(sensor_names.begin(), sensor_names.end(), "lo_locked") != sensor_names.end()) {
        uhd::sensor_value_t lo_locked = usrp->get_rx_sensor("lo_locked",0);
        std::cout << boost::format("Checking RX: %s ...") % lo_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(lo_locked.to_bool());
    }
    sensor_names = usrp->get_mboard_sensor_names(0);
    if ((ref == "mimo") and (std::find(sensor_names.begin(), sensor_names.end(), "mimo_locked") != sensor_names.end())) {
        uhd::sensor_value_t mimo_locked = usrp->get_mboard_sensor("mimo_locked",0);
        std::cout << boost::format("Checking RX: %s ...") % mimo_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(mimo_locked.to_bool());
    }
    if ((ref == "external") and (std::find(sensor_names.begin(), sensor_names.end(), "ref_locked") != sensor_names.end())) {
        uhd::sensor_value_t ref_locked = usrp->get_mboard_sensor("ref_locked",0);
        std::cout << boost::format("Checking RX: %s ...") % ref_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(ref_locked.to_bool());
    }

   //set mode flags
    unsigned int nMode;
    if (mode=="avmfft"){
    nMode=1;
    }
    if (mode=="cmpfft"){
    nMode=2;
    }
    if (mode=="tmsmps"){
    nMode=3;
    }
    if (mode=="chnsts"){
    nMode=4;
    }
    
    //create a receive streamer
    uhd::stream_args_t stream_args("fc32"); //complex floats
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

    // rm// setup streaming ... 0 means continues 
     uhd::stream_cmd_t stream_cmd((total_num_samps == 0)?
     uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS:
     uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE
    );
    stream_cmd.num_samps = total_num_samps;// total_num_samps=0 means coninuous mode  
    stream_cmd.stream_now = true;
    stream_cmd.time_spec = uhd::time_spec_t();
    usrp->issue_stream_cmd(stream_cmd);

    //loop until total number of samples reached
    size_t num_acc_samps = 0; //number of accumulated samples
    size_t  nAvrgCount = 0;
    uhd::rx_metadata_t md;
    
    std::vector<std::complex<float> > buff(num_bins);
    std::vector<std::complex<float> > out_buff(num_bins);
    std::vector<float> out_buff_norm(num_bins);
   /* struct vBuff
    {
    std::vector<float> send_avmfft;
    std::vector<std::complex<float> > send_cmpfft; 
    std::vector<std::complex<float> > send_tmsmps;
    // chnsts could be boolean vector rather than int
    std::vector<unsigned short> send_chnsts;
	};*/
	
    std::vector<float> send_avmfft(num_bins-Moving_Avg_size);
    // there is actually no need for the two below vectors since send_cmpfft equal out of fft buff
    // and send_tmsmps equal to buff from USRP, I will leave like this for clarity and in case we need
    // extra calculations before sending the buff
    std::vector<std::complex<float> > send_cmpfft(num_bins); 
    std::vector<std::complex<float> > send_tmsmps;
    
    //initializing sum of channels matrix
    // calculating number of channels in chnsts mode
    unsigned int nChs,nSlize;
    nChs=static_cast <int> (std::floor((rate/2)/chbw));
    nSlize=static_cast <int> (std::floor((num_bins/2)/nChs));
    
    
    std::vector<float> vChCusum(nChs,0);
    // create chnsts buffer to send this could be boolean vector also
    std::vector<unsigned short> send_chnsts(nChs,0);
    
    //initialize fft plan 
    fftwf_complex *in = (fftwf_complex*)&buff.front();
    fftwf_complex *out = (fftwf_complex*)&out_buff.front();
    fftwf_plan p;
    p = fftwf_plan_dft_1d(num_bins, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    std::cout << "refresh interval = " << long(1e6*num_bins/rate)  << std::endl;
    std::cout << "fft bins = " << num_bins << std::endl;
    std::cout << "window size = " << Moving_Avg_size << std::endl;
    
    //initialize udp connectrion to send
    uhd::transport::udp_simple::sptr udp_xport = uhd::transport::udp_simple::make_connected(addr, port);
    
    
    if (total_num_samps == 0){
        std::signal(SIGINT, &sig_int_handler);
        std::cout << "Slected mode "<<mode<<"     Press Ctrl + C to stop streaming..." << std::endl;
   
   // intitialize ncurses for terminal display, may not need it for uhd_daemon
   initscr();
  // attron(A_BOLD);
   attron(A_STANDOUT);
   start_color();
   init_pair(1, COLOR_YELLOW, COLOR_RED);
   attron(COLOR_PAIR(1));
   std::string frame;
   // end of ncurses intialization
   
   //main loop
   
    while(not stop_signal_called and (num_acc_samps < total_num_samps or total_num_samps == 0)){
        size_t num_rx_samps = rx_stream->recv(
            &buff.front(), buff.size(), md, 3.0
        );
        
        //handle the error codes
        switch(md.error_code){
        case uhd::rx_metadata_t::ERROR_CODE_NONE:
            break;

        case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
            if (num_acc_samps == 0) continue;
            std::cout << boost::format(
                "Got timeout before all samples received, possible packet loss, exiting loop..."
            ) << std::endl;
            goto done_loop;

        default:
            std::cout << boost::format(
                "Got error code 0x%x, exiting loop..."
            ) % md.error_code << std::endl;
            goto done_loop;
        }
 // selecting mode of operation
  switch (nMode)
    {
		// mode avmfft
		case 1:
			fftwf_execute(p);
			//calculate avmfft from moving average function
		    send_avmfft=Moving_Avg(out_buff,Moving_Avg_size);
		    // send_avmfft buffer to udp connection
		    udp_xport->send(boost::asio::buffer(send_avmfft));
		    num_acc_samps += num_rx_samps;
         break;
         // mode cmpfft
         case 2:
		    fftwf_execute(p);
		    send_cmpfft=out_buff;
		    //send_cmpfft buffer to udp connection
		    udp_xport->send(boost::asio::buffer(send_cmpfft));
		    num_acc_samps += num_rx_samps;
         break;
         //mode tmsmps
        case 3:
		    send_tmsmps=buff;  
		    //send_tmsmps buffer to udp connection
		    udp_xport->send(boost::asio::buffer(send_tmsmps));
		    num_acc_samps += num_rx_samps;  
        break;
	    case 4:  
		    if (nAvrgCount<navrg)
			    {
				fftwf_execute(p);
				for (unsigned int i=0; i<out_buff.size();i++)
			         out_buff_norm[i]=pow(abs(out_buff[i]),2);
			         //out_buff_norm[i]=std::norm(out_buff[i]);
			    std::vector<float>::iterator it1;//=out_buff_norm.begin();
			    std::vector<float>::iterator it2;//=out_buff_norm.begin();
			    //auto it1=out_buff_norm.begin();
			    //auto it2=out_buff_norm.begin();
				for( unsigned int ch_i=0;ch_i<nChs;ch_i++)
				{
					it1=out_buff_norm.begin()+ ch_i*nSlize;
					it2=out_buff_norm.begin()+ (ch_i+1)*nSlize-1;
					//std::advance(it1,static_cast <int>(std::floor(num_bins/2))+ ch_i*nSlize);
					//std::advance(it2,static_cast <int>(std::floor(num_bins/2))+(ch_i+1)*nSlize);
					//it1=out_buff_norm.begin()+static_cast <int>(std::floor(num_bins/2))+ ch_i*nSlize;
					//it2=out_buff_norm.begin()+static_cast <int>(std::floor(num_bins/2))+ (ch_i+1)*nSlize;
					//test output the selected elments
				    /*std::ostream_iterator<float> output(std::cout," ");
			        std::copy(it1,it2,output);
			        std::cout<<"\n \n"<<std::endl;*/
			        //if (ch_i==0) {vChCusum[ch_i]=vChCusum[ch_i]-*(it1)/nSlize;} //subtract DC component
			        while ( it1!=it2 ) {vChCusum[ch_i] = vChCusum[ch_i] + 2*(*it1++)/nSlize;}
			    }
			    nAvrgCount++;
			    }
		  // after nuber of averages is done calculate channel status    
	       else
	           { 
			    //std::cout<<nChs<<std::endl;
			    for( unsigned int ch_j=0;ch_j<nChs;ch_j++)
			    {
				  if ((vChCusum[ch_j]/nAvrgCount) >thresh) //threshold 
				    {
				    send_chnsts[ch_j]=1;
				    //std::cout<<"Ch"<<ch_i<<" "<<send_chnsts[ch_i]<<"\t";
				    }
				  else
				    {
					send_chnsts[ch_j]=0;	
					}
				 // constructing text frame for ncurses display	
		         frame+="Ch"+boost::lexical_cast<std::string>(ch_j)+" "+boost::lexical_cast<std::string>(send_chnsts[ch_j])+"\t"+boost::lexical_cast<std::string>(vChCusum[ch_j]/nAvrgCount)+"\n";//+"  "+boost::lexical_cast<std::string>(nAvrgCount)+"\t";
			     vChCusum[ch_j]=0;
			    }
			    // displaying on screen, ncurses
				clear();
				//std::setprecision(5);
				frame="Selected mode "+mode+". Starting at frequency "+boost::lexical_cast<std::string>(freq/1e6)+"MHz. \n Total bandwidth is " +boost::lexical_cast<std::string>(rate/2/1e6)+"Mhz sliced to "+boost::lexical_cast<std::string>(nChs)+" channels \n with channel bandwidth equal to "+ boost::lexical_cast<std::string>(chbw/1e3)+"KHz. \n Press Ctrl + C to stop streaming...\n______________________________\n                              \n"+frame;
		        printw("%s", frame.c_str());
				refresh();  
				frame.erase (frame.begin(), frame.end());   
		        // end of ncurses frame 
		        nAvrgCount=0;
	         }
	     //send the final result send_chnsts to udp connection
	     udp_xport->send(boost::asio::buffer(send_chnsts));
	     num_acc_samps += num_rx_samps; 
	    break;
       default:
       std::cout<<" Error no mode was selected"<<std::endl;
       break;
    }
    }
    } done_loop:
    
    if ((mode == "avmfft") or (mode == "cmpfft") or (mode=="chnsts"))
    fftwf_destroy_plan(p);
    //finished
    std::cout<<"\n"<<std::endl;
    //std::cout<<"Buffer size"<< buff.size()<<std::endl;
    std::cout << std::endl << "Done!" << std::endl << std::endl;
    // deleting ncurses window
    endwin();
    return 0;
}
