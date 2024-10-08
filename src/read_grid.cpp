/* ----------------------------------------------------------------------
   SPARTA - Stochastic PArallel Rarefied-gas Time-accurate Analyzer
   http://sparta.github.io
   Steve Plimpton, sjplimp@gmail.com, Michael Gallis, magalli@sandia.gov
   Sandia National Laboratories

   Copyright (2014) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level SPARTA directory.
------------------------------------------------------------------------- */

#include "stdlib.h"
#include "string.h"
#include "read_grid.h"
#include "grid.h"
#include "domain.h"
#include "comm.h"
#include "input.h"
#include "hash3.h"
#include "memory.h"
#include "error.h"

using namespace SPARTA_NS;

#define MAXLINE 256
#define CHUNK 1024
#define DELTA_CUSTOM 8192

enum{INT,DOUBLE};                      // several files

/* ---------------------------------------------------------------------- */

ReadGrid::ReadGrid(SPARTA *sparta) : Pointers(sparta)
{
  line = new char[MAXLINE];
  keyword = new char[MAXLINE];
  buffer = new char[CHUNK*MAXLINE];
}

/* ---------------------------------------------------------------------- */

ReadGrid::~ReadGrid()
{
  delete [] line;
  delete [] keyword;
  delete [] buffer;
}

/* ----------------------------------------------------------------------
   called as read_grid command in input script
------------------------------------------------------------------------- */

void ReadGrid::command(int narg, char **arg)
{
  if (!domain->box_exist)
    error->all(FLERR,"Cannot read grid before simulation box is defined");
  if (grid->exist)
    error->all(FLERR,"Cannot read grid when grid is already defined");

  grid->exist = 1;

  if (narg < 1) error->all(FLERR,"Illegal read_grid command");

  // optional args

  int iarg = 1;

  ncustom = 0;
  name_custom = NULL;
  type_custom = NULL;
  size_custom = NULL;
  nvalues_custom = 0;

  while (iarg < narg) {
    if (strcmp(arg[iarg],"custom") == 0) {
      if (iarg+4 > narg) error->all(FLERR,"Invalid read_grid command");

      name_custom = (char **)
	memory->srealloc(name_custom,(ncustom+1)*sizeof(char *),
			 "readgrid:name_custom");
      memory->grow(type_custom,ncustom+1,"readgrid:type_custom");
      memory->grow(size_custom,ncustom+1,"readgrid:size_custom");

      int n = strlen(arg[iarg+1]) + 1;
      name_custom[ncustom] = new char[n];
      strcpy(name_custom[ncustom],arg[iarg+1]);
      if (strcmp(arg[iarg+2],"int") == 0) type_custom[ncustom] = INT;
      else if (strcmp(arg[iarg+2],"float") == 0) type_custom[ncustom] = DOUBLE;
      else error->all(FLERR,"Invalid read_grid command");
      size_custom[ncustom] = input->inumeric(FLERR,arg[iarg+3]);
      if (size_custom[ncustom] < 0)
	error->all(FLERR,"Invalid read_surf command");

      if (size_custom[ncustom] == 0) nvalues_custom++;
      else nvalues_custom += size_custom[ncustom];

      ncustom++;

      iarg += 4;
    } else error->all(FLERR,"Invalid read_grid command");
  }

  // read file

  read(arg[0],0);
}

/* ----------------------------------------------------------------------
   called from command()
   use of external = 1 would allow calling it from another command
------------------------------------------------------------------------- */

void ReadGrid::read(char *filename, int external)
{
  // read file, create parent cells and then child cells

  MPI_Barrier(world);
  double time1 = MPI_Wtime();

  me = comm->me;
  nprocs = comm->nprocs;

  if (me == 0) {
    if (!external && screen)
      fprintf(screen,"Reading grid file ... %s\n",filename);
    open(filename);
  }

  // read header and Cells section

  header();
  parse_keyword(1);
  if (strcmp(keyword,"Cells") != 0)
    error->all(FLERR,
               "Read_grid did not find Cells section of grid file");
  read_cells();

  // close file

  if (me == 0) {
    if (compressed) pclose(fp);
    else fclose(fp);
  }

  // new per-proc grid cell count has now been set
  // create and populate any custom per-grid vectors and arrays

  if (ncustom) {
    create_custom();
    memory->destroy(cvalues);
    for (int ic = 0; ic < ncustom; ic++) delete [] name_custom[ic];
    memory->sfree(name_custom);
    memory->destroy(type_custom);
    memory->destroy(size_custom);
  }

  // invoke grid methods to complete grid setup

  MPI_Barrier(world);
  double time2 = MPI_Wtime();

  grid->set_maxlevel();
  grid->setup_owned();
  grid->acquire_ghosts();
  grid->find_neighbors();
  grid->check_uniform();
  comm->reset_neighbors();

  MPI_Barrier(world);
  double time3 = MPI_Wtime();

  // stats

  if (external) return;

  double time_total = time3-time1;

  if (comm->me == 0) {
    if (screen) {
      fprintf(screen,"  grid cells = " BIGINT_FORMAT "\n",grid->ncell);
      fprintf(screen,"  CPU time = %g secs\n",time_total);
      fprintf(screen,"  read/setup percent = %g %g\n",
              100.0*(time2-time1)/time_total,100.0*(time3-time2)/time_total);
    }

    if (logfile) {
      fprintf(logfile,"  grid cells = " BIGINT_FORMAT "\n",grid->ncell);
      fprintf(logfile,"  CPU time = %g secs\n",time_total);
      fprintf(logfile,"  read/setup percent = %g %g\n",
              100.0*(time2-time1)/time_total,100.0*(time3-time2)/time_total);
    }
  }
}

/* ----------------------------------------------------------------------
   read/store all child cells
------------------------------------------------------------------------- */

void ReadGrid::read_cells()
{
  int i,m,nchunk;

  // read and broadcast one CHUNK of lines at a time

  whichproc = 0;
  bigint nread = 0;

  nclocal = ncmax = 0;
  cvalues = NULL;

  bigint count = 0;
  while (nread < ncell) {
    if (ncell-nread > CHUNK) nchunk = CHUNK;
    else nchunk = ncell-nread;
    if (me == 0) {
      char *eof;
      m = 0;
      for (i = 0; i < nchunk; i++) {
        eof = fgets(&buffer[m],MAXLINE,fp);
        if (eof == NULL) error->one(FLERR,"Unexpected end of grid file");
        m += strlen(&buffer[m]);
      }
      if (buffer[m-1] != '\n') strcpy(&buffer[m++],"\n");
      m++;
    }
    MPI_Bcast(&m,1,MPI_INT,0,world);
    MPI_Bcast(buffer,m,MPI_CHAR,0,world);

    // add occasional barrier to prevent issues from having too many
    //  outstanding MPI recv requests (from the broadcast above)

    if (count % 1024 == 0) MPI_Barrier(world);

    create_cells(nchunk,buffer);
    nread += nchunk;
    count++;
  }

  grid->ncell = ncell;

  if (me == 0) {
    if (screen) fprintf(screen,"  " BIGINT_FORMAT " grid cells\n",grid->ncell);
    if (logfile) fprintf(logfile,"  " BIGINT_FORMAT " grid cells\n",grid->ncell);
  }
}

/* ----------------------------------------------------------------------
   create one child cell per line of Cells section of grid file
------------------------------------------------------------------------- */

void ReadGrid::create_cells(int n, char *buf)
{
  int level;
  cellint id;
  char *next,*idptr;
  double lo[3],hi[3];

  // nwords_required = # of words per line

  int nwords_required = 1 + nvalues_custom;
  double *custom = new double[nvalues_custom];

  // create one child cell for each line
  // assign to procs in round-robin fasion

  double *boxlo = domain->boxlo;
  double *boxhi = domain->boxhi;

  int i,ic,iv,icvalue;

  for (i = 0; i < n; i++) {
    next = strchr(buf,'\n');

    if (me == whichproc) {
      *next = '\0';
      int nwords = input->count_words(buf);
      *next = '\n';

      if (nwords != nwords_required)
	error->one(FLERR,"Incorrect line format in grid file");

      idptr = strtok(buf," \t\n\r\f");
      id = ATOCELLINT(idptr);
      if (id < 0) error->all(FLERR,"Invalid cell ID in grid file");

      if (ncustom) {
	icvalue = 0;
	for (ic = 0; ic < ncustom; ic++) {
	  if (type_custom[ic] == INT) {
	    if (size_custom[ic] == 0)
	      custom[icvalue++] = input->inumeric(FLERR,strtok(NULL," \t\n\r\f"));
	    else
	      for (iv = 0; iv < size_custom[ic]; iv++)
		custom[icvalue++] = input->inumeric(FLERR,strtok(NULL," \t\n\r\f"));
	  } else if (type_custom[ic] == DOUBLE) {
            if (size_custom[ic] == 0)
              custom[icvalue++] = input->numeric(FLERR,strtok(NULL," \t\n\r\f"));
            else
              for (iv = 0; iv < size_custom[ic]; iv++)
	      custom[icvalue++] = input->numeric(FLERR,strtok(NULL," \t\n\r\f"));
          }
        }
      }

      level = grid->id_level(id);
      if (level < 0) error->one(FLERR,"Cell ID in grid file exceeds maxlevel");
      grid->id_lohi(id,level,boxlo,boxhi,lo,hi);
      grid->add_child_cell(id,level,lo,hi);
      add_custom(custom);
    }

    whichproc++;
    if (whichproc == nprocs) whichproc = 0;

    buf = next + 1;
  }

  // clean up custom value storage

  delete [] custom;
}

/* ----------------------------------------------------------------------
   append custom values for one grid cell to cvalues array
   grow cvalues as needed
------------------------------------------------------------------------- */

void ReadGrid::add_custom(double *custom)
{
  if (nclocal == ncmax) {
    ncmax += DELTA_CUSTOM;
    memory->grow(cvalues,ncmax,nvalues_custom,"readgrid:cvalues");
  }

  for (int ivalue = 0; ivalue < nvalues_custom; ivalue++)
    cvalues[nclocal][ivalue] = custom[ivalue];

  nclocal++;
}

/* ----------------------------------------------------------------------
   create custom per-grid vectors and arrays
   cannot be done until after grid file is read and grid->nlocal is valid
     b/c grid->add_custom() uses nlocal to allocate per-grid vector/array
   populate each vector or array with read-in cvalues
------------------------------------------------------------------------- */

void ReadGrid::create_custom()
{
  int i,j,index;

  int nlocal = grid->nlocal;
  int icvalue = 0;

  for (int ic = 0; ic < ncustom; ic++) {
    index = grid->add_custom(name_custom[ic],type_custom[ic],size_custom[ic]);

    if (type_custom[ic] == INT) {
      if (size_custom[ic] == 0) {
	int *ivector = grid->eivec[grid->ewhich[index]];
	for (i = 0; i < nlocal; i++)
	  ivector[i] = static_cast<int> (cvalues[i][icvalue]);
	icvalue++;
      } else {
	int **iarray = grid->eiarray[grid->ewhich[index]];
	for (i = 0; i < nlocal; i++)
	  for (j = 0; j < size_custom[ic]; j++)
	    iarray[i][j] = static_cast<int> (cvalues[i][icvalue+j]);
	icvalue += size_custom[ic];
      }

    } else if (type_custom[ic] == DOUBLE) {
      if (size_custom[ic] == 0) {
	double *dvector = grid->edvec[grid->ewhich[index]];
	for (i = 0; i < nlocal; i++)
	  dvector[i] = cvalues[i][icvalue];
	icvalue++;
      } else {
	double **darray = grid->edarray[grid->ewhich[index]];
	for (i = 0; i < nlocal; i++)
	  for (j = 0; j < size_custom[ic]; j++)
	    darray[i][j] = cvalues[i][icvalue+j];
	icvalue += size_custom[ic];
      }
    }
  }
}

/* ----------------------------------------------------------------------
   proc 0 opens grid file
   test if gzipped
------------------------------------------------------------------------- */

void ReadGrid::open(char *file)
{
  compressed = 0;
  char *suffix = file + strlen(file) - 3;
  if (suffix > file && strcmp(suffix,".gz") == 0) compressed = 1;
  if (!compressed) fp = fopen(file,"r");
  else {
#ifdef SPARTA_GZIP
    char gunzip[128];
    sprintf(gunzip,"gunzip -c %s",file);
    fp = popen(gunzip,"r");
#else
    error->one(FLERR,"Cannot open gzipped file");
#endif
  }

  if (fp == NULL) {
    char str[128];
    sprintf(str,"Cannot open file %s",file);
    error->one(FLERR,str);
  }
}

/* ----------------------------------------------------------------------
   read free-format header of grid file
   1st line and blank lines are skipped
   non-blank lines are checked for header keywords and leading value is read
   header ends with non-blank line containing no header keyword (or EOF)
   return line with non-blank line (or empty line if EOF)
------------------------------------------------------------------------- */

void ReadGrid::header()
{
  int n,ilevel,nx,ny,nz;
  char *ptr;

  // skip 1st line of file

  if (me == 0) {
    char *eof = fgets(line,MAXLINE,fp);
    if (eof == NULL) error->one(FLERR,"Unexpected end of grid file");
  }

  ncell = 0;
  nlevels = 0;
  Level *levels = NULL;

  while (1) {

    // read a line and bcast length

    if (me == 0) {
      if (fgets(line,MAXLINE,fp) == NULL) n = 0;
      else n = strlen(line) + 1;
    }
    MPI_Bcast(&n,1,MPI_INT,0,world);

    // if n = 0 then end-of-file so return with blank line

    if (n == 0) {
      line[0] = '\0';
      return;
    }

    // bcast line

    MPI_Bcast(line,n,MPI_CHAR,0,world);

    // trim anything from '#' onward
    // if line is blank, continue

    if ((ptr = strchr(line,'#'))) *ptr = '\0';
    if (strspn(line," \t\n\r") == strlen(line)) continue;

    // search line for header keywords and set corresponding variables

    if (strstr(line,"cells")) {
      sscanf(line,BIGINT_FORMAT,&ncell);
    } else if (strstr(line,"levels")) {
      sscanf(line,"%d",&nlevels);
      if (nlevels <= 0) error->all(FLERR,"Grid file levels must be > 0");
      if (nlevels > grid->plevel_limit)
        error->all(FLERR,"Grid file levels exceeds MAXLEVEL");
      delete [] levels;
      levels = new Level[nlevels];
      for (int i = 0; i < nlevels; i++) levels[i].setflag = 0;
    } else if (strstr(line,"level-")) {
      if (!levels) error->all(FLERR,"Grid file levels is not set");
      ptr = strstr(line,"level-");
      ptr += strlen("level-");
      ilevel = atoi(ptr);
      if (ilevel < 1 || ilevel > nlevels)
        error->all(FLERR,"Grid file level-N is invalid");
      sscanf(line,"%d %d %d",&nx,&ny,&nz);
      if (levels[ilevel-1].setflag == 1)
        error->all(FLERR,"Grid file level-N is already set");
      levels[ilevel-1].setflag = 1;
      levels[ilevel-1].cx = nx;
      levels[ilevel-1].cy = ny;
      levels[ilevel-1].cz = nz;
    } else break;
  }

  // error checks

  if (ncell == 0) error->all(FLERR,"Grid file does not set cells keyword");
  if (nlevels == 0) error->all(FLERR,"Grid file does not set levels keyword");
  for (int i = 0; i < nlevels; i++) {
    if (!levels[i].setflag) error->all(FLERR,"Grid file does not set all levels");
    if (domain->dimension == 2 && levels[i].cz != 1)
      error->all(FLERR,"Read_grid nz value must be 1 for a 2d simulation");
    if (levels[i].cx < 1 || levels[i].cy < 1 || levels[i].cz < 1)
      error->all(FLERR,"Read_grid nx,ny,nz cannot be < 1");
    if (levels[i].cx == 1 && levels[i].cy == 1 && levels[i].cz == 1)
      error->all(FLERR,"Read_grid nx,ny,nz cannot all be one");
  }

  // transfer level info into Grid data structs

  Grid::ParentLevel *plevels = grid->plevels;
  grid->maxlevel = nlevels;

  for (int i = 0; i < nlevels; i++) {
    plevels[i].nx = levels[i].cx;
    plevels[i].ny = levels[i].cy;
    plevels[i].nz = levels[i].cz;
    plevels[i].nxyz = (bigint) levels[i].cx * levels[i].cy * levels[i].cz;
  }

  for (int i = 0; i < nlevels; i++) {
    if (i == 0) plevels[i].nbits = 0;
    else plevels[i].nbits = plevels[i-1].nbits + plevels[i-1].newbits;
    plevels[i].newbits = grid->id_bits(plevels[i].nx,plevels[i].ny,plevels[i].nz);
  }

  // error check on too many bits for cell IDs

  int nbits = plevels[nlevels-1].nbits + plevels[nlevels-1].newbits;
  if (nbits > sizeof(cellint)*8) {
    char str[128];
    sprintf(str,"Hierarchical grid induces cell IDs that exceed %d bits",
            (int) sizeof(cellint)*8);
    error->all(FLERR,str);
  }

  // clean up

  delete [] levels;
}

/* ----------------------------------------------------------------------
   grab next keyword
   read lines until one is non-blank
   keyword is all text on line w/out leading & trailing white space
   read one additional line (assumed blank)
   if any read hits EOF, set keyword to empty
   if first = 1, line variable holds non-blank line that ended header
------------------------------------------------------------------------- */

void ReadGrid::parse_keyword(int first)
{
  int eof = 0;

  // proc 0 reads upto non-blank line plus 1 following line
  // eof is set to 1 if any read hits end-of-file

  if (me == 0) {
    if (!first) {
      if (fgets(line,MAXLINE,fp) == NULL) eof = 1;
    }
    while (eof == 0 && strspn(line," \t\n\r") == strlen(line)) {
      if (fgets(line,MAXLINE,fp) == NULL) eof = 1;
    }
    if (fgets(buffer,MAXLINE,fp) == NULL) eof = 1;
  }

  // if eof, set keyword empty and return

  MPI_Bcast(&eof,1,MPI_INT,0,world);
  if (eof) {
    keyword[0] = '\0';
    return;
  }

  // bcast keyword line to all procs

  int n;
  if (me == 0) n = strlen(line) + 1;
  MPI_Bcast(&n,1,MPI_INT,0,world);
  MPI_Bcast(line,n,MPI_CHAR,0,world);

  // copy non-whitespace portion of line into keyword

  int start = strspn(line," \t\n\r");
  int stop = strlen(line) - 1;
  while (line[stop] == ' ' || line[stop] == '\t'
         || line[stop] == '\n' || line[stop] == '\r') stop--;
  line[stop+1] = '\0';
  strcpy(keyword,&line[start]);
}
