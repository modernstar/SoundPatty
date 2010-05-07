/*  
 *  readit.cpp
 *
 *  Copyright (c) 2010 Motiejus Jakštys
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.

 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.

 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <math.h>
#include <algorithm>
#include <stdlib.h>
#include <cstdio>
#include <map>
#include <set>
#include <list>
#include <vector>
#include <string>
#include <string.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <jack/jack.h>

#define BUFFERSIZE 1

using namespace std;

FILE * process_headers(const char*);
bool check_sample(const char*, const char*);
void search_wav_patterns(FILE * in, void (*callback)(int, double, double, unsigned long), double timeout);
void search_patterns (jack_default_audio_sample_t * buf, jack_nframes_t nframes, void (*callback)(int, double, double, unsigned long));
void read_periods(const char*);
void do_checking (int, double, double, unsigned long);
void read_cfg(const char*, const int);
void fatal (const char* );
void fatal2(void * r);
int jack_proc(jack_nframes_t nframes, void *arg);
vector<string> explode(const string&, const string&);

char errmsg[200];   // Temporary message
int SAMPLE_RATE,    // 
    WAVE,           // SAMPLE_RATE*MINWAVEFREQ
    CHUNKSIZE,      // CHUNKLEN*SAMPLE_RATE
    DATA_SIZE;      // Data chunk size in bytes

map<string, double> cfg; // OK, ok... Is it possible to store any value but string (Rrrr...) here?

// Length of patterns
class Range {
    public:
        Range() { tm = tmin = tmax = 0; };
        Range(const double & a) { tm = a; tmin = a * 0.89; tmax = a * 1.11; }
        Range(const double & tmin, const double &tm, const double &tmax) { this->tmin = tmin; this->tm = tm; this->tmax = tmax; }
        Range(const double & tmin, const double &tmax) { this->tmin = tmin; this->tmax = tmax; }
        Range operator = (const double i) { return Range(i); }
        bool operator == (const double & i) const { return tmin < i && i < tmax; }
        bool operator == (const Range & r) const { return r == tm; }
        bool operator > (const double & i) const { return i > tmax; }
        bool operator > (const Range & r) const { return r > tm; }
        bool operator < (const double & i) const { return i < tmin; }
        bool operator < (const Range & r) const { return r < tm; }
        double tmin, tm, tmax;
};


// Volume (-1..1 or -2^15..2^15)
struct sVolumes {
    unsigned long head, tail;
	jack_default_audio_sample_t min, max;
    bool proc;
};
vector<sVolumes> volume;

class workitm {
    public:
        int len,a;
        unsigned long b;

        workitm(int, unsigned long);
        list<pair<int, unsigned long> > trace;
};

workitm::workitm(const int a, const unsigned long b) {
    this->a = a; this->b = b;
    len = 0;
    trace.push_back(pair<int,unsigned long>(a,b));
};

void its_over(workitm * w, double place) {
    printf("FOUND, processed %.6f sec\n", place);
    /*
       printf("Found a matching pattern! Length: %d, here comes the trace:\n", (int)w->trace.size());
       for (list<pair<int,unsigned long> >::iterator tr = w->trace.begin(); tr != w->trace.end(); tr++) {
       printf("%d %d\n", tr->first, tr->second);
       }
     */
    exit(0);
}

struct valsitm {
    int c; // Counter in map
    double place;
};

typedef multimap<pair<int, Range>, valsitm> tvals;
tvals vals;
list<workitm> work;
unsigned long gMCounter = 0; // How many matches we found
unsigned long gSCounter = 0; // How many samples we skipped
jack_port_t * dst_port;
char * src_port_n;

jack_client_t * client;

void dump_out(int w, double place, double len, unsigned long noop) {
    printf ("%d;%.6f;%.6f\n", w, place, len);
}

void new_port(const jack_port_id_t port_id, int registerr, void *arg) {
    jack_port_t * src_port = jack_port_by_id(client, port_id);
    if (registerr) {
        if (JackPortIsOutput & jack_port_flags(src_port)) {
            printf("Connecting %s with %s\n", jack_port_name(src_port), jack_port_name(dst_port));
			char buffer[200];
			// Hacky. Must use thread + FIFO queue for this.
            sprintf(buffer, "sleep 0.5 && jack_connect %s %s &", jack_port_name(src_port), jack_port_name(dst_port));
            system(buffer);
        }
    }
}

int main (int argc, char *argv[]) {
    if (argc < 3) {
        fatal ("Usage: ./readit config.cfg sample.wav\nor\n"
                "./readit config.cfg samplefile.txt catchable.wav\n"
                "./readit config.cfg samplefile.txt jack jack");
    }
    // Another :HACK: to write config values.
    // If argc == 5, then we are running JACK,
    // but JACK stores volume in signed float (-1..1).
    // Else we are reading WAV, therefore values are (-2^15..2^15)
    int multiply = (argc == 5)?1:(1<<15);

    read_cfg(argv[1], multiply);
    // ------------------------------------------------------------
    // Only header given, we just dump out the values
    //
    if (argc == 3) {
        FILE * in = process_headers(argv[2]);
        search_wav_patterns(in, dump_out, cfg["sampletimeout"]);
    }
    // ------------------------------------------------------------
    // We have values file and a new samplefile. Let's rock
    //
    else if (argc == 4 || argc == 5) {
        ifstream file;
        file.open(argv[2]);
        string line;
        for (int i = 0; !file.eof(); i++) {
            getline(file, line);
            if (line.size() == 0) break;
            vector<string> numbers = explode(";", line);
            istringstream num(numbers[0]);
            istringstream place(numbers[1]);
            istringstream range(numbers[2]);

            double tmp2;
            pair<pair<int, Range>,valsitm > tmp;
            num >> tmp2; tmp.first.first = tmp2; // Index in volume
            range >> tmp2; tmp.first.second = Range(tmp2);
            place >> tmp.second.place; // Place in the stream
            tmp.second.c = i; // Counter in the stream
            vals.insert(tmp);
        }
    }
    if (argc == 4) {
        // OK, we have it. Let's print?
        //for (vector<trange>::iterator it1 = vals.begin(); it1 != vals.end(); it1++) {
        //printf ("%.6f - %.6f\n", (*it1).first, (*it1).second);
        //}
        // ------------------------------------------------------------
        // Let's work with the seconds sample like with a stream
        //
        FILE * an = process_headers(argv[3]);
        search_wav_patterns(an, do_checking, cfg["catchtimeout"]);
        printf("NOT FOUND\n");

    } else if (argc == 5) {

        char dst_chan_name[] = "sp_";
        strcat(dst_chan_name, argv[4]);
        if ((client = jack_client_new (dst_chan_name)) == 0) {
            fatal ("jack server not running?\n");
        }

        jack_set_process_callback(client, jack_proc, 0);
        jack_on_shutdown(client, fatal2, (void*)"Jack server shut us down!");

        SAMPLE_RATE = jack_get_sample_rate (client);
		WAVE = (int)SAMPLE_RATE * cfg["minwavelen"];
		CHUNKSIZE = cfg["chunklen"] * SAMPLE_RATE;
        dst_port = jack_port_register (client, "input", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
		src_port_n = (char*) malloc(strlen(argv[4])+1);
		strcpy(src_port_n, argv[4]);

		if (jack_set_port_registration_callback(client, new_port, NULL)) {
			printf("Setting client registration callback failed\n");
		}

        if (jack_activate (client)) {
            fatal ("cannot activate client");
        }

		/*
        if (jack_connect(client, argv[4], jack_port_name(dst_port))) {
            fatal("Failed to connect two ports!\n");
        }
		*/

        while (1) {
			if (gSCounter/SAMPLE_RATE > cfg["catchtimeout"]) {
				printf("NOT FOUND, catchtimeout reached\n");
				if (jack_disconnect(client, src_port_n, jack_port_name(dst_port))) {
					printf("Failed to disconnect ports...!\n");
				}
				exit(0);
			}

            sleep (1);
        }
    }
    exit(0);
}

// --------------------------------------------------------------------------------
// This gets called every time there is a treshold found.
// Work array is global
//
// int r - id of treshold found (in config.cfg: treshold_(<?r>\d)_(min|max))
// double place - place of sample from the stream start (sec)
// double sec - length of a found sample (sec)
// int b - index (overall) of sample found
//
void do_checking (int r, double place, double sec, unsigned long b) {

    //pair<tvals::iterator, tvals::iterator> pa = vals.equal_range(pair<int,double>(r,sec));
    // Manually searching for matching values because with that pairs equal_range doesnt work
    // Iterate through pa

    tvals fina; // FoundInA
    Range demorange(sec);
    pair<int,Range> sample(r,demorange);
    for (tvals::iterator it1 = vals.begin(); it1 != vals.end(); it1++) {
        if (it1->first == sample)
            fina.insert(*it1);
    }
    //------------------------------------------------------------
    // We put a indexes here that we use for continued threads
    // (we don't want to create a new "thread" with already
    // used length of a sample)
    //
    set<int> used_a; 

    //------------------------------------------------------------
    // Iterating through samples that match the found sample
    //
    for (tvals::iterator in_a = fina.begin(); in_a != fina.end(); in_a++)
    {
        //printf("%d %.6f matches %.6f (%d)\n", in_a->first.first, sec, in_a->first.second.tm, in_a->second.c);
        int a = in_a->second.c;
        //------------------------------------------------------------
        // Check if it exists in our work array
        //
        for (list<workitm>::iterator w = work.begin(); w != work.end();) {
            if (b - w->b > round(cfg["maxsteps"])) {
                work.erase(w); w = work.begin(); continue;
            }
            if (b == w->b || a - w->a > round(cfg["maxsteps"]) || w->a >= a) { w++; continue; }
            // ------------------------------------------------------------
            // We fit the "region" here. We either finished,
            // or just increasing len
            //
            w->a = a; w->b = b;
            w->trace.push_back(pair<int,unsigned long>(a,b));
            if (++(w->len) < round(cfg["matchme"])) { // Proceeding with the "thread"
                used_a.insert(a);
                //printf ("Thread expanded to %d\n", w->len);
            } else { // This means the treshold is reached
                // This hack is needed to pass pointer to an instance o_O
                its_over(&(*w), place+sec);
            }
            w++;
            // End of work iteration array
        }
        if (used_a.find(a) == used_a.end()) {
            work.push_back(workitm(a,b));
            //printf ("Pushed back %d %d\n", a,b);
        }
    }
    //printf("\n");
}

void read_cfg (const char * filename, const int multiply) {
    ifstream file;
    file.open(filename);
    string line;
    int x;
    while (! file.eof() ) {
        getline(file, line);
        x = line.find(":");
        if (x == -1) break; // Last line, exit
        istringstream i(line.substr(x+2));
        double tmp; i >> tmp;
        cfg[line.substr(0,x)] = tmp;
    }
    // Change cfg["treshold\d+_(min|max)"] to
    // something more compatible with sVolumes map
    sVolumes tmp;
    tmp.head = tmp.tail = tmp.max = tmp.min = tmp.proc = 0;
    volume.assign(cfg.size(), tmp); // Make a bit more then nescesarry
    int max_index = 0; // Number of different tresholds
    for(map<string, double>::iterator C = cfg.begin(); C != cfg.end(); C++) {
        // Failed to use boost::regex :(
        if (C->first.find("treshold") == 0) {
            istringstream tmp(C->first.substr(8));
            int i; tmp >> i;
            max_index = max(max_index, i);
            // C->second and volume[i].m{in,ax} are double
            if (C->first.find("_min") != -1) {
                volume[i].min = C->second*multiply;
            } else {
                volume[i].max = C->second*multiply;
            }
        }
    }
    volume.assign(volume.begin(), volume.begin()+max_index+1); // Because [start,end), but we need all
}

FILE * process_headers(const char * infile) {
    FILE * in = fopen(infile, "rb");

    // Read bytes [0-3] and [8-11], they should be "RIFF" and "WAVE" in ASCII
    char header[5];
    fread(header, 1, 4, in);

    char sample[] = "RIFF";
    if (! check_sample(sample, header) ) {
        sprintf(errmsg, "RIFF header not found in %s, exiting\n", infile);
		fatal(errmsg);
    }
    // Checking for compression code (21'st byte is 01, 22'nd - 00, little-endian notation
    uint16_t tmp[2]; // two-byte integer
    fseek(in, 0x14, 0); // offset = 20 bytes
    fread(&tmp, 2, 2, in); // Reading two two-byte samples (comp. code and no. of channels)
    if ( tmp[0] != 1 ) {
        sprintf(errmsg, "Only PCM(1) supported, compression code given: %d\n", tmp[0]);
        fatal (errmsg);
    }
    // Number of channels must be "MONO"
    if ( tmp[1] != 1 ) {
        sprintf(errmsg, "Only MONO supported, given number of channels: %d\n", tmp[1]);
        fatal (errmsg);
    }
    fread(&SAMPLE_RATE, 2, 1, in); // Single two-byte sample
    WAVE = (int)SAMPLE_RATE*cfg["minwavelen"];
    CHUNKSIZE = cfg["chunklen"]*SAMPLE_RATE;
	fseek(in, 0x22, 0);
	uint16_t BitsPerSample;
	fread(&BitsPerSample, 1, 2, in);
	if (BitsPerSample != 16) {
		sprintf(errmsg, "Only 16-bit WAVs supported, given: %d\n", BitsPerSample);
		fatal(errmsg);
	}
    // Get data chunk size here
    fread(header, 1, 4, in);
	strcpy(sample, "data");

    if (! check_sample(sample, header)) {
        fatal ("data chunk not found in byte offset=36, file corrupted.");
    }
    fread(&DATA_SIZE, 4, 1, in); // Single two-byte sample
    //printf ("Datasize: %d bytes\n", DATA_SIZE);
    return in;
}

bool check_sample (const char * sample, const char * b) { // My STRCPY.
    for(int i = 0; sample[i] != '\0'; i++) {
        if (sample[i] != b[i]) {
            return false;
        }
    }
    return true;
}

void fatal (const char * msg) { // Print error message and exit
    printf (msg);
    exit (1);
}

void fatal2(void * r) {
    char * msg = (char*) r;
    fatal(msg);
}

int jack_proc(jack_nframes_t nframes, void *arg) {
    jack_default_audio_sample_t *in = (jack_default_audio_sample_t *) jack_port_get_buffer (dst_port, nframes);

    search_patterns(in, nframes, do_checking);
    return 0;
}

void search_wav_patterns(FILE * in, void (*callback)(int, double, double, unsigned long), double timeout) {
    int16_t buf [SAMPLE_RATE * BUFFERSIZE]; // Process buffer every BUFFERSIZE secs
    while (!feof(in)) {
        if (gSCounter/SAMPLE_RATE > timeout) break;
        fread(buf, 2, SAMPLE_RATE * BUFFERSIZE, in);

        // :HACK: fix this, do not depend on jack types where you don't need!
        jack_default_audio_sample_t buf2 [SAMPLE_RATE * BUFFERSIZE];
        for(int i = 0; i < SAMPLE_RATE * BUFFERSIZE; i++) {
            buf2[i] = (jack_default_audio_sample_t)buf[i];
        }
        search_patterns(buf2, SAMPLE_RATE * BUFFERSIZE, callback);
    }
}

void search_patterns (jack_default_audio_sample_t * buf, jack_nframes_t nframes, void (*callback)(int, double, double, unsigned long)) {

    for (int i = 0; i < nframes; gSCounter++, i++) {
        jack_default_audio_sample_t cur = buf[i]<0?-buf[i]:buf[i];

        int v = 0; // Counter for volume
        for (vector<sVolumes>::iterator V = volume.begin(); V != volume.end(); V++, v++) {
            if (V->min <= cur && cur <= V->max) {
                // ------------------------------------------------------------
                // If it's first item in this wave (proc = processing started)
                //
                if (!V->proc) {
                    V->tail = gSCounter;
                    V->proc = true;
                }
                // ------------------------------------------------------------
                // Here we are just normally in the wave.
                //
                V->head = gSCounter;
            } else { // We are not in the wave
                if (V->proc && (V->min < 0.001 || gSCounter - V->head > WAVE)) {

                    //------------------------------------------------------------
                    // This wave is over
                    //
                    if (gSCounter - V->tail >= CHUNKSIZE) {
                        // ------------------------------------------------------------
                        // The previous chunk is big enough to be noticed
                        //
                        callback(v, (double)V->tail/SAMPLE_RATE, (double)(V->head - V->tail)/SAMPLE_RATE, gMCounter++);
                    } 
                    // ------------------------------------------------------------
                    // Else it is too small, but we don't want to do anything in that case
                    // So therefore we just say that wave processing is over
                    //
                    V->proc = false;
                }
            }
        }
    }
}

vector<string> explode(const string &delimiter, const string &str) // Found somewhere on NET
{
    vector<string> arr;
    int strleng = str.length();
    int delleng = delimiter.length();
    if (delleng == 0)
        return arr; //no change
    int i = 0, k = 0;
    while (i < strleng) {
        int j = 0;
        while (i+j < strleng && j < delleng && str[i+j] == delimiter[j])
            j++;
        if (j == delleng) {
            arr.push_back(str.substr(k, i-k));
            i += delleng;
            k = i;
        } else {
            i++;
        }
    }
    arr.push_back(str.substr(k, i-k));
    return arr;
};
