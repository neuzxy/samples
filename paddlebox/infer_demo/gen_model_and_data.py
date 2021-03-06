from __future__ import print_function
import os
import paddle.fluid as fluid
import paddle.fluid.layers as layers
import random
import string

num_slots = 20
emb_size = 11

def rand_name(length=4):
    letters = string.ascii_lowercase
    return ''.join(random.choice(letters) for i in range(length))

slots = [rand_name() + '_' + str(i) for i in range(num_slots)]

data_dir = 'data'
if not os.path.isdir(data_dir):
    os.makedirs(data_dir)

with open(data_dir + '/sample.data', 'w') as fout:
    for slot in slots:
        print('%s\t%s' % (slot, ' '.join(['%.2f' % random.random() for i in range(emb_size)])), file=fout)

train_program = fluid.Program()
start_program = fluid.Program()
with fluid.program_guard(train_program, start_program):
    bows = []
    for slot in slots:
        bow = fluid.layers.data(name=slot, shape=[emb_size], dtype='float32')
        bows.append(bow)

    bow_sum = layers.sum(bows)
    data_norm = layers.data_norm(input=bow_sum)
    fc1 = layers.fc(input=data_norm, size=8, act='relu')
    #print(fc1.name) # fc_0.tmp_2
    fc2 = layers.fc(input=fc1, size=1)
    #print(fc2.name) # fc_1.tmp_1
    sigmoid = layers.sigmoid(fc2)

print('\nall variables:')
for var in train_program.current_block().vars:
    print(var)

print('\nall parameters:')
for param in train_program.current_block().all_parameters():
    print(param.name)


if __name__ == '__main__':
    exe = fluid.Executor(fluid.CPUPlace())
    exe.run(start_program)

    save_dir = 'model'
    if not os.path.isdir(save_dir):
        os.makedirs(save_dir)

    infer_program = train_program.clone()
    fluid.io.save_inference_model(dirname=save_dir,
                                  main_program=infer_program,
                                  model_filename='program.bin',
                                  params_filename='model.bin',
                                  feeded_var_names=slots,
                                  target_vars=sigmoid,
                                  executor=exe)

