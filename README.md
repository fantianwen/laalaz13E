# Document for Leela_Zero


## Introduction for this project

### Motivation

As the original Leela Zero(https://github.com/leela-zero/leela-zero) was written in C++, and if you have already read the paper "[AlphaGo Zero](https://deepmind.com/research/publications/mastering-game-go-without-human-knowledge)", the code reading of Leela Zero will be joyful.
 
* Modifying code for various researches
* Training an AlphaGo Zero model by yourself 

### Branches
The original repository is here: https://github.com/fantianwen/laalaz13E

As you can see, there are many branches in the projects.

- master: the branch which is in track with the original project (https://github.com/leela-zero/leela-zero), 19x19 board-size version
- ter_and_inf: the branch which works for "Various Strategies Production";
- normal: normal 13x13 board-size Leela Zero version;
- develop: position control development (the branch name is not good, sorry);
- simple_strength_control: using simple position control method;
- mix_C: various strategy production by using mixed method.(C means center)

These are the main branches used in researches.


### Researches

By using this project, mainly two topic have been done.


#### Position Control
 As the development of Go Programs, the existence of AlphaGo and AlphaGoZero has made the strength of Go Programs beyond human pro-players.
  So, researches about how to entertain and coach human players are yet to be interesting and important topic.  
 One important topic is to lose naturally during the match playing. So a 
  * NOT so strong Go program
  * Go program plays little UNNATURAL move

  is the expected Go program.
   
   
#### Various Strategies Production

Some interesting strategies are using in the real Go playing by human players.
Such as "center-oriented" or "edge/corner-oriented".


## Working method of this project

### C/S mode
C/S mode means "Client/Server" mode, Here Leela Zero will work as a server after compiling into a executable file.
The client side is GTP(Go Text Protocol).

### GTP (Go Text Protocol)
Please refer to [GTP in sensei](https://senseis.xmp.net/?GTP) for the detailed explanation of GTP.

In brief, after one server of Go Program is started, there is channel that client(GTP) can send message to server directly, So the Go program can response and reply the respective result to the client.

By using this mode of exchanging information, the Client/Server need only focus on how to implement functions.

> for example, if send a message "genmove b" to server, after receiving the message, server will generate the next move and "tell" the client directly. All the things that client need to do is "sending message", "waiting" and "receiving result".

### The drawbacks of using GTP

Although introduced some good points by using GTP, still some troubles when using in the reality.

#### Hard to debug
Although we can know the mistakes if operating executable file directly, but working as a server, if a command from GTP is wrongly executed inner Leela, It will only throw out some message that is hard to understand.
Here maybe the explanation is here to understand, but if you begin to "play with" Leela, you will know what I want to say.

I have thought about how to solve this problems but have not started.

* Inner the Leela, try to catch exceptions or mistakes by yourself, and throw the exception outside to log file. When something is wrong, you can refer to the log for solution.


### How the C/S mode works

As in the research, one usual but important operation is to generate game records by two Go programs.
We have written auto fight scripts in Python, we will introduce you how the C/S mode works by the auto-file script.

#### auto-fight 

A simple script if auto-fight is here "https://github.com/fantianwen/ExTools/blob/master/autoFight.py" for referring.

I will not explain here in detail. It is easy to understand, please read the script and you will know how it works fast.

Ok, now you have know how this script is working. 

In the class "GTPFacade", there are many field functions. Such as "def play(self, color, vertex)", "def showboard(self)" and so on.

These functions use some GTP commands which is already existed in the Leela or some other Go programs. 
But what if we need to get some other information from Leela when running.


##### write your own GTP command(s)
The default GTP commands may be not enough, so we need to know how to write and implement your needed GTP commands.

###### At the side of Leela Zero

In the file "GTP.cpp", add the new command.

###### At the side of Client

A client here is the python script.
Please write new GTP command in the class "GTPFacade".


## Training new model

- check to branch "ter_and_inf"
- under the file folder: https://github.com/fantianwen/laalaz13E/tree/ter_and_inf/training/tf/
- run "minitrain.sh", it will generate game records
- please ensure the generated game records are under the same folder
- run "trainpipe.sh", it will use the generated game records for training


