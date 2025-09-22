#include<iostream>
#include<vector>
#include<string>
#include<cstring>
#include<sstream>
#include<sys/wait.h> // wait()
#include<unistd.h> // fork(), execvp()
#include<signal.h>
#include<fcntl.h> // open()

using namespace std;

void sigchild_handler(int signal){
    while (waitpid(-1, NULL, WNOHANG) > 0);
    // reap children
}

int main(){

    string cmd_line;
    signal (SIGCHLD, sigchild_handler);

    while(true){
        cout << ">";
        getline(cin, cmd_line);
        if(cmd_line == "exit")break;

        // cut the input command
        stringstream ss(cmd_line);
        string token;
        vector<char*> args;
        vector<char*> sec_args;
        bool neglect = false; // if end with &
        bool second_cmd = false;
        bool redirect = false;
        string re_type;
        string outfile, infile; // cmd < infile, cmd > outfile
        while(ss >> token) {
            if(token == "|"){
                second_cmd = true;
                continue;
            }
            if(token == "<" || token == ">"){
                redirect = true;
                re_type = token;
                continue;
            }
            if(second_cmd || redirect)sec_args.push_back(strdup(token.c_str()));
            else args.push_back(strdup(token.c_str()));
        }
        // if then take away &
        if(!args.empty() && strcmp(args.back(), "&") == 0){
            neglect = true;
            free(args.back());
            args.pop_back();
        }
        if(!sec_args.empty() && strcmp(sec_args.back(), "&") == 0){
            neglect = true;
            free(sec_args.back());
            sec_args.pop_back();
        }
        args.push_back(nullptr);

        if(!sec_args.empty() && sec_args[0] != nullptr && second_cmd){
            sec_args.push_back(nullptr);
            int pi[2];
            //pipe(pi); pi[0] is read, pi[1] is write
            if(pipe(pi) == -1){
                perror("pipe error");
                for(char* arg: args){
                    if(arg)free(arg);
                }
                for(char* arg: sec_args){
                    if(arg)free(arg);
                }
                continue;
            }
            //child 1
            pid_t pid_1 = fork();
            if(pid_1==0){
                close(pi[0]);
                dup2(pi[1], STDOUT_FILENO);
                close(pi[1]);
                execvp(args[0], args.data());
                perror("args1 failed");
                exit(1);
            }

            //child 2
            pid_t pid_2 = fork();
            if(pid_2==0){
                close(pi[1]);
                dup2(pi[0], STDIN_FILENO);
                close(pi[0]);
                execvp(sec_args[0], sec_args.data());
                perror("sec_args failed");
                exit(1);
            }

            //parent
            close(pi[0]);
            close(pi[1]);
            if(!neglect){
                waitpid(pid_1, NULL, 0);
                waitpid(pid_2, NULL, 0);    
            }   
        }
        else if(!sec_args.empty() && sec_args[0] != nullptr && redirect){
            sec_args.push_back(nullptr);
            if(re_type == "<"){
                infile = sec_args[0];
            }
            else if(re_type == ">"){
                outfile = sec_args[0];
            }
            pid_t pid_r = fork();
            if(pid_r==0){
                if(!outfile.empty()){
                    int fd = open(outfile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
                    if(fd == -1){
                        perror("opening output file error\n");
                        exit(1);
                    }
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }
                if(!infile.empty()){
                    int fd = open(infile.c_str(), O_RDONLY);
                    if(fd == -1){
                        perror("open input file error\n");
                        exit(1);
                    }
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                }
                execvp(args[0], args.data());
                perror("redirection failed");
                exit(1);
            }
            else if(pid_r > 0){
                if(!neglect){
                    waitpid(pid_r, NULL, 0);
                }
            }
        }
        else{
            pid_t pid = fork();

            if(pid < 0){ // error
                fprintf(stderr, "Fork Failed\n");
                exit(-1);
            }
            else if (pid == 0){ // child
                // do the child
                execvp(args[0], args.data());
                perror("perror failed");
                exit(1);
            }
            else{
                if(!neglect)wait(NULL); 
            }
        }
        
        for (char* arg : args) {
            if(arg)free(arg);
        }
        for (char* arg : sec_args){
            if(arg)free(arg);
        }
    }
    return 0;
}
