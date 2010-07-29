/*
 *  biascorrection.cpp
 *  cufflinks
 *
 *  Created by Adam Roberts on 5/20/10.
 *  Copyright 2010 __MyCompanyName__. All rights reserved.
 *
 */

#include "biascorrection.h"
#include "abundances.h"

#include <iostream>
#include <fstream>

void colSums(const ublas::matrix<long double>& A, vector<long double>& sums)
{
	sums = vector<long double>(A.size2(),0.0);
	for (int i = 0; i < A.size1(); ++i)
		for (int j = 0; j < A.size2(); ++j)
			sums[j] += A(i,j);
}

void fourSums(const ublas::matrix<long double>& A, ublas::matrix<long double>& sums)
{
	sums = ublas::zero_matrix<long double>(A.size1(), A.size2()/4);
	for (int i = 0; i < A.size1(); ++i)
		for (int j = 0; j < A.size2(); ++j)
			sums(i,j/4) += A(i,j);
}

void ones(ublas::matrix<long double>& A)
{
	for (int i = 0; i < A.size1(); ++i)
		for (int j = 0; j < A.size2(); ++j)
			A(i,j) = 1;
}

void get_compatibility_list(const vector<Scaffold>& transcripts,
						 const vector<MateHit>& alignments,
						 vector<list<int> >& compatibilities)
{
	int M = alignments.size();
	int N = transcripts.size();
	
	vector<Scaffold> alignment_scaffs;
	
	for (size_t i = 0; i < alignments.size(); ++i)
	{
		const MateHit& hit = alignments[i];
		alignment_scaffs.push_back(Scaffold(hit));
	} 
	
	for (int i = 0; i < M; ++i) 
	{
		for (int j = 0; j < N; ++j) 
		{
			if (transcripts[j].strand() != CUFF_STRAND_UNKNOWN
				&& transcripts[j].contains(alignment_scaffs[i]) 
				&& Scaffold::compatible(transcripts[j],alignment_scaffs[i]))
			{
				compatibilities[i].push_back(j);
			}
		}
	}
}


bool learn_bias(BundleFactory& bundle_factory, BiasLearner& bl)
{
	bundle_factory.load_ref_rnas(true, true);

	HitBundle bundle;
	
	int num_bundles = 0;

	RefSequenceTable& rt = bundle_factory.hit_factory().ref_table();

	while(true)
	{
		HitBundle* bundle_ptr = new HitBundle();
		
		if (!bundle_factory.next_bundle(*bundle_ptr))
		{
			delete bundle_ptr;
			break;
		}
		
		HitBundle& bundle = *bundle_ptr;
		
		if (bundle.right() - bundle.left() > 3000000)
		{
			fprintf(stderr, "Warning: large bundle encountered...\n");
		}
		if (bundle.hits().size())
		{
			num_bundles++;

			fprintf(stderr, "Learning bias[ %s:%d-%d ] with %d non-redundant alignments\n", 
					rt.get_name(bundle.ref_id()),
					bundle.left(),
					bundle.right(),
					(int)bundle.non_redundant_hits().size());
			
			
			const vector<Scaffold>& transcripts = bundle.ref_scaffolds();
			const vector<MateHit>& alignments = bundle.non_redundant_hits();
			
			
			int M = alignments.size();
			int N = transcripts.size();
			
			if (N==0)
			{
				delete bundle_ptr;
				continue;
			}
			
			vector<list<int> > compatibilities(M,list<int>());
			
			get_compatibility_list(transcripts, alignments, compatibilities);	
			
			vector<vector<long double> > startHists(N+1, vector<long double>()); // +1 to catch overhangs
			vector<vector<long double> > endHists(N+1, vector<long double>());
			
			
			for (int j = 0; j < N; ++j)
			{
				int size = transcripts[j].length();
				startHists[j].resize(size);
				endHists[j].resize(size);
			}

			for (int i = 0; i < M; ++i)
			{
				const MateHit& hit = alignments[i];
				if (!hit.left_alignment() && !hit.right_alignment())
					continue;
				
				
				double num_hits = bundle.collapse_counts()[i];
				long double locus_fpkm = 0;
				
				for (list<int>::iterator it=compatibilities[i].begin(); it!=compatibilities[i].end(); ++it)
				{
					locus_fpkm += transcripts[*it].fpkm(); 
				}
				
				if (locus_fpkm==0)
					continue;
				
				for (list<int>::iterator it=compatibilities[i].begin(); it!=compatibilities[i].end(); ++it)
				{	
					const Scaffold& transcript = transcripts[*it];
					assert(transcript.strand()!=CUFF_STRAND_UNKNOWN); //Filtered in compatibility list
					
					if (!hit.is_pair())
					{
						assert(hit.left_alignment()); // Singletons should always be left.
						bool antisense = hit.left_alignment()->antisense_align();
						if (antisense && transcript.strand()==CUFF_FWD || !antisense && transcript.strand() == CUFF_REV)
						{
							int g_end  = (transcript.strand()==CUFF_FWD) ? hit.right()-1:hit.left();
							int s_end = transcript.genomic_to_transcript_coord(g_end);
							endHists[*it][s_end] += num_hits*transcript.fpkm()/locus_fpkm;
						}
						else
						{
							int g_start = (transcript.strand()==CUFF_FWD) ? hit.left():hit.right()-1;
							int s_start = transcript.genomic_to_transcript_coord(g_start);
							startHists[*it][s_start] += num_hits*transcript.fpkm()/locus_fpkm;
						}
					}
					else 
					{
						pair<int, int> t_span = transcript.genomic_to_transcript_span(hit.genomic_outer_span());
						startHists[*it][t_span.first] += num_hits*transcript.fpkm()/locus_fpkm;
						endHists[*it][t_span.second] += num_hits*transcript.fpkm()/locus_fpkm;
					}
				}
			}
			for (int j = 0; j < N; ++j)
			{
				if (transcripts[j].strand()!=CUFF_STRAND_UNKNOWN && transcripts[j].fpkm() > 0)
					bl.processTranscript(startHists[j], endHists[j], transcripts[j]);
			}
		}
		delete bundle_ptr;
	}
	bl.normalizeParameters();
	bl.output();
	return true;
}

const int BiasLearner::pow4[] = {1,4,16,64};
//Fdriconst int BiasLearner::paramTypes[] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
const int BiasLearner::paramTypes[] = {1,1,1,1,1,2,2,2,3,3,3,3,3,3,3,2,2,2,1,1,1}; //Length of connections at each position in the window
const int BiasLearner::MAX_SLICE = 3; // Maximum connection length
const int BiasLearner::CENTER = 8; //Index in paramTypes[] of first element in read
const int BiasLearner::_M = 21; //Number of positions spanned by window
const int BiasLearner::_N = 64; //Length of maximum connection in VLMM
const int BiasLearner::lengthBins[] = {791,1265,1707,2433}; //Quantiles derived from human mRNA length distribution in UCSC genome browser
const double BiasLearner::positionBins[] = {.02,.04,.06,.08,.10,.15,.2,.25,.3,.35,.4,.5,.6,.7,.75,.8,.85,.9,.95,1};
//const double BiasLearner::positionBins[] = {0.01,0.02,0.03,0.04,0.05,0.06,0.07,0.08,0.09,0.10,0.11,0.12,0.13,0.14,0.15,0.16,0.17,0.18,0.19,0.20,0.21,0.22,0.23,0.24,0.25,0.26,0.27,0.28,0.29,0.30,0.31,0.32,0.33,0.34,0.35,0.36,0.37,0.38,0.39,0.40,0.41,0.42,0.43,0.44,0.45,0.46,0.47,0.48,0.49,0.50,0.51,0.52,0.53,0.54,0.55,0.56,0.57,0.58,0.59,0.60,0.61,0.62,0.63,0.64,0.65,0.66,0.67,0.68,0.69,0.70,0.71,0.72,0.73,0.74,0.75,0.76,0.77,0.78,0.79,0.80,0.81,0.82,0.83,0.84,0.85,0.86,0.87,0.88,0.89,0.90,0.91,0.92,0.93,0.94,0.95,0.96,0.97,0.98,0.99,1};

BiasLearner::BiasLearner(const EmpDist& frag_len_dist)
{
	_frag_len_dist = frag_len_dist;
	_startParams = ublas::zero_matrix<long double>(_M,_N);
	_startExp = ublas::zero_matrix<long double>(_M,_N);
	_endParams = ublas::zero_matrix<long double>(_M,_N);
	_endExp = ublas::zero_matrix<long double>(_M,_N);
	_posParams = ublas::zero_matrix<long double>(20,5);
	_posExp = ublas::zero_matrix<long double>(20,5);
}


inline int BiasLearner::seqToInt(const char* seqSlice, int n)
{
	int c = 0;
	for(int i = 0; i < n; i++)
	{	
		if (seqSlice[i] == 4) return -1;//N
		c += (seqSlice[i])*pow4[n-i-1];
	}
	return c;
}

inline void BiasLearner::getSlice(const char* seq, char* slice, int start, int end) // INCLUSIVE!
{
	if (end >= start)
	{
		for (int i = start; i <= end; ++i)
		{
			slice[i-start] = seq[i];
		}
	}
	else 
	{
		for(int i = start; i >= end; --i)
		{
			slice[start-i] = seq[i];
		}
	}
}

void BiasLearner::processTranscript(const std::vector<long double>& startHist, const std::vector<long double>& endHist, const Scaffold& transcript)
{
	double fpkm = transcript.fpkm();
	int seqLen = transcript.length();
	
	char seq[seqLen];
	char c_seq[seqLen];
	encode_seq(transcript.seq(), seq, c_seq);
	
	char seqSlice[MAX_SLICE];
	
	int lenClass=0;
	while (seqLen > lengthBins[lenClass] && lenClass < 4)
	{
		lenClass++;
	}
	
	int currBin = 0;
	int binCutoff = positionBins[currBin]*seqLen;
		
	for (int i=0; i < seqLen; i++)
	{
		
		//Position Bias
		if (i > binCutoff)
			binCutoff=positionBins[++currBin]*seqLen;
		_posParams(currBin, lenClass) += startHist[i]/fpkm;
		_posExp(currBin, lenClass) += _frag_len_dist.cdf(seqLen-i);
		
		
		bool start_in_bounds = i-CENTER >= 0 && i+(_M-1)-CENTER < seqLen;
		bool end_in_bounds = i+CENTER-(_M-1) >= 0 && i+CENTER < seqLen;
		
		if (!start_in_bounds && !end_in_bounds) // Make sure we are in bounds of the sequence
			continue;
		
		//Sequence Bias
		for(int j=0; j < _M; j++)
		{
			// Start Bias
			if (start_in_bounds) // Make sure we are in bounds of the sequence
			{
				int k = i+j-CENTER;
				getSlice(seq, seqSlice, k-(paramTypes[j]-1), k);
				int v = seqToInt(seqSlice,paramTypes[j]);
				if (v >= 0)
				{
					_startParams(j,v) += startHist[i]/fpkm;
					_startExp(j,v) += _frag_len_dist.cdf(seqLen-i);
				}
				else // There is an N.  Average over all possible values of N
				{
					list<int> nList(1,0);
					genNList(seqSlice, 0, paramTypes[j],nList);
					for (list<int>::iterator it=nList.begin(); it!=nList.end(); ++it)
					{
						_startParams(j,*it) += startHist[i]/(fpkm * (double)nList.size());
						_startExp(j,*it) += _frag_len_dist.cdf(seqLen-i)/nList.size();
					}
				}
			}
			// End Bias
			if (end_in_bounds) // Make sure we are in bounds of the sequence
			{
				int k = i+CENTER-j;
				getSlice(c_seq, seqSlice, k+(paramTypes[j]-1), k); 
				int v = seqToInt(seqSlice, paramTypes[j]);
				if (v >= 0)
				{
					_endParams(j,v) += endHist[i]/fpkm;
					_endExp(j,v) += _frag_len_dist.cdf(i+1);
				}
				else // There is an N.  Average over all possible values of N
				{
					list<int> nList(1,0);
					genNList(seqSlice, 0, paramTypes[j], nList);
					for (list<int>::iterator it=nList.begin(); it!=nList.end(); ++it)
					{
						_endParams(j,*it) += endHist[i]/(fpkm * (double)nList.size());
						_endExp(j,*it) += _frag_len_dist.cdf(i+1)/nList.size();
					}
				}
			}
		}
	}
}


void BiasLearner::genNList(const char* seqSlice, int start, int n, list<int>& nList)
{

	if (n > 1) 
		genNList(seqSlice, start+1, n-1, nList);
	
	if (seqSlice[start]==4)
	{
		for (int i = nList.size()-1; i>=0; --i)
		{
			for (int j=0; j<4; ++j) 
			{
				nList.push_back(nList.front()+j*pow4[n-1]);
			}
			nList.pop_front();
		}
	}
	else 
	{
		for (list<int>::iterator it=nList.begin(); it!=nList.end(); ++it)
			(*it)+=seqSlice[start]*pow4[n-1];
	}
	
}

void BiasLearner::normalizeParameters()
{
	//Normalize position parameters	
	//vector<long double> posParam_sums;
	vector<long double> posExp_sums;
	colSums(_posParams, posParam_sums); // Total starts for each length class
	colSums(_posExp, posExp_sums); // Total FPKM for each length class
	
	for(int i=0; i < _posParams.size1(); i++)
		for(int j=0; j < _posParams.size2(); j++)
		{
			_posParams(i,j) /= posParam_sums[j];
			_posExp(i,j) /= posExp_sums[j];
			_posParams(i,j) /= _posExp(i,j);
		}
	
	ublas::matrix<long double> startExp_sums;
	ublas::matrix<long double> startParam_sums;
	fourSums(_startParams, startParam_sums);
	fourSums(_startExp, startExp_sums);
	
	ublas::matrix<long double> endExp_sums;
	ublas::matrix<long double> endParam_sums;
	fourSums(_endParams, endParam_sums);
	fourSums(_endExp, endExp_sums); 
	
	//Normalize sequence parameters
	for(int i=0; i < _M; i++)
		for(int j=0; j < pow4[paramTypes[i]]; j++)
		{
			if (startParam_sums(i,j/4)==0)
			{
				_startParams(i,j) = 1;
			}
			else 
			{
				_startParams(i,j) /= startParam_sums(i,j/4);
				_startExp(i,j) /= startExp_sums(i,j/4);
				_startParams(i,j) /= _startExp(i,j);
			}
			if (endParam_sums(i,j/4)==0)
			{
				_endParams(i,j) = 1;
			}
			else 
			{
				_endParams(i,j) /= endParam_sums(i,j/4);
				_endExp(i,j) /= endExp_sums(i,j/4);
				_endParams(i,j) /= _endExp(i,j);
			}

		}
	ones(_posParams);
}

void BiasLearner::output()
{
	ofstream myfile1;
	ofstream myfile2;
	ofstream myfile3;
	ofstream myfile4;
	string startfile = output_dir + "/startBias.csv";
	myfile1.open (startfile.c_str());
	string endfile = output_dir + "/endBias.csv";
	myfile2.open (endfile.c_str());
	string posfile = output_dir + "/posBias.csv";
	myfile3.open (posfile.c_str());
	string posfile2 = output_dir + "/posExp.csv";
	myfile4.open (posfile2.c_str());


	for (int i = 0; i < _N; ++i)
	{
		for(int j = 0; j < _M; ++j)
		{
			myfile1 << _startParams(j,i) <<",";
			myfile2 << _endParams(j,i) << ",";
		}
		myfile1 << endl;
		myfile2 << endl;
	}
	
	for (int i = 0; i < _posParams.size2(); ++i)
	{
		for(int j = 0; j < _posParams.size1(); ++j)
		{
			myfile3 << _posParams(j,i) <<",";
			myfile4 << _posExp(j,i) <<",";
		}
		//myfile3 << "," << posParam_sums[i];
		myfile3 <<endl;
		myfile4 <<endl;
	}
	
	myfile1.close();
	myfile2.close();
	myfile3.close();
	myfile4.close();
}

void BiasLearner::getBias(const Scaffold& transcript, vector<double>& startBiases, vector<double>& endBiases)
{
	int seqLen = transcript.length();
	
	char seq[seqLen];
	char c_seq[seqLen];
	encode_seq(transcript.seq(), seq, c_seq);
	
	char seqSlice[MAX_SLICE];
	
	int lenClass=0;
	while (seqLen > lengthBins[lenClass] && lenClass < 4)
	{
		lenClass++;
	}
	
	int currBin = 0;
	int binCutoff = positionBins[currBin]*seqLen;
	
	for (int i=0; i < seqLen; i++)
	{
		//Position Bias
		if (i > binCutoff)
			binCutoff= positionBins[++currBin]*seqLen;
		double posBias = _posParams(currBin, lenClass);
		
		
		//Sequence Bias
		double startBias = 1;
		double endBias = 1;
		
		bool start_in_bounds = i-CENTER >= 0 && i+(_M-1)-CENTER < seqLen;
		bool end_in_bounds = i+CENTER-(_M-1) >= 0 && i+CENTER < seqLen;
		
		if (!start_in_bounds && !end_in_bounds) // Make sure we are in bounds of the sequence
		{
			for(int j=0; j < _M; j++)
			{
				// Start Bias
				if (start_in_bounds) // Make sure we are in bounds of the sequence
				{
					int k = i+j-CENTER;
					getSlice(seq, seqSlice, k-(paramTypes[j]-1), k);
					int v = seqToInt(seqSlice, paramTypes[j]);
					if (v >= 0)
					{
						startBias *= _startParams(j,v);
					}
					else // There is an N.  Average over all possible values of N
					{
						list<int> nList(1,0);
						double tot = 0;
						genNList(seqSlice, 0, paramTypes[j],nList);

						for (list<int>::iterator it=nList.begin(); it!=nList.end(); ++it)
						{
							tot += _startParams(j,*it);
						}
						startBias *= tot/nList.size();
					}
				}
				
				// End Bias
				if (end_in_bounds) // Make sure we are in bounds of the sequence
				{
					int k = i+CENTER-j;
					getSlice(c_seq, seqSlice, k+(paramTypes[j]-1), k);
					int v = seqToInt(seqSlice,paramTypes[j]);
					if (v >= 0)
					{
						endBias *= _endParams(j,v);
					}
					else // There is an N.  Average over all possible values of N
					{
						list<int> nList(1,0);
						double tot = 0;
						genNList(seqSlice, 0, paramTypes[j],nList);
						for (list<int>::iterator it=nList.begin(); it!=nList.end(); ++it)
						{
							tot += _endParams(j,*it);
						}
						endBias *= tot/nList.size();
					}
				}
			}
		}
		startBiases[i] = startBias*posBias;
		endBiases[i] = endBias;
	}
}
