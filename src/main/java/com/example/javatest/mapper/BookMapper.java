package com.example.javatest.mapper;

import com.example.javatest.entity.Book;
import org.apache.ibatis.annotations.Mapper;
import java.util.List;

@Mapper
public interface BookMapper {

    List<Book> findAll();

    Book findById(Long id);

    int insert(Book book);

    int update(Book book);

    int deleteById(Long id);
}
