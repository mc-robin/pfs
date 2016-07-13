# pfs
	A filesystem driver for linux, the sourceforge is broken, so i move the project to github.
	I try to design a filesystem and write a driver for linux, it's an interesting experiment, 
now the driver can barely work.
	The pfs filesytem isn't a performance filesytem, the p is initials of pokemom. pfs don't use bit map 
or extent tree, it use array(logically linked list) to organize data block. the directory is described by hash list, 
block-to-block map are used by pfs. inodes and blocks are alloced by same mechanism.
	Now, it's a test verson, if you have any suggestions or find bugs, please contact me(nnsmgsone@gmail.com).
	Read README for more information.
