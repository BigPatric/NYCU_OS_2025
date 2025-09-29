#include<iostream>
#include<unistd.h>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<sys/wait.h>
#include<sys/time.h>

using namespace std;

// shmget()
// shmat()
// shmdt()
// shmctl()
// gettimeofday()

// checksum : sum up all elements with unsigned 32-bit integer

int main(){

    struct timeval start, end;
    int dimension;
    cout << "Input the matrix dimension: ";
    cin >> dimension;

    // create shared memory
    int shmid_A = shmget(IPC_PRIVATE, sizeof(int)*dimension*dimension, IPC_CREAT | 0666);
    int shmid_B = shmget(IPC_PRIVATE, sizeof(int)*dimension*dimension, IPC_CREAT | 0666);
    int shmid_C = shmget(IPC_PRIVATE, sizeof(int)*dimension*dimension, IPC_CREAT | 0666);

    int *A = (int *)shmat(shmid_A, NULL, 0);
    int *B = (int *)shmat(shmid_B, NULL, 0);
    int *C = (int *)shmat(shmid_C, NULL, 0);

    // fill the matrix
    for(int i = 0; i < dimension; i++){
        for(int j = 0; j < dimension; j++){
            A[i * dimension + j] = i * dimension + j;
            B[i * dimension + j] = i * dimension + j;
            C[i * dimension + j] = 0;
        }
    }

    for (int Proc = 1; Proc <= 16; Proc++){

        for(int i = 0; i < dimension; i++){
            for(int j = 0; j < dimension; j++){
                C[i * dimension + j] = 0;
            }
        }

        cout << "Multiplying matrices using " << Proc << " processes" << endl;
        gettimeofday(&start, NULL);

        int rows_per_process = dimension / Proc;
        int extra_rows = dimension % Proc;
        // first fairly distributing then the rest

        pid_t pids[Proc];

        for(int p = 0; p<Proc; p++){
            pids[p] = fork();
            if(pids[p] < 0){
                cerr << "Fork Failed" << endl;
                exit(1);
            }
            else if(pids[p] == 0){ // child process
                
                int start_row = p * rows_per_process + min(p, extra_rows);
                int end_row = start_row + rows_per_process - 1;
                if (p < extra_rows) end_row++;
                for(int i = start_row; i <= end_row; i++){
                    for(int j = 0; j < dimension; j++){
                        C[i * dimension + j] = 0;
                        for(int k = 0; k < dimension; k++){
                            C[i * dimension + j] += A[i * dimension + k] * B[k * dimension + j];
                        }
                    }
                }
                shmdt(A);
                shmdt(B);
                shmdt(C);
                exit(0);
            }
        }

        for(int p = 0; p < Proc; p++){
            waitpid(pids[p], NULL, 0);
        }

        unsigned int checkSum = 0;
        for(int i = 0; i < dimension; i++){
            for(int j = 0;j < dimension; j++){
                checkSum += C[i * dimension + j];
            }
        }
        gettimeofday(&end, NULL);
        long elapsed = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
        double elapsed_sec = elapsed / 1000000.0;
        cout << "Elapsed time: " << elapsed_sec << " sec";
        cout << "Checksum: " << checkSum << endl;
    }

    shmctl(shmid_A, IPC_RMID, NULL);
    shmctl(shmid_B, IPC_RMID, NULL);
    shmctl(shmid_C, IPC_RMID, NULL);

    return 0;
}